// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::BufRead;
use std::io::BufReader;
use std::path::Path;
use std::path::PathBuf;

use anyhow::Context;
use anyhow::Result;
use config_file::ConfigFile;
use oxidized::custom_error_config::CustomErrorConfig;
use oxidized::experimental_features;
use oxidized::global_options::ExtendedReasonsConfig;
use oxidized::global_options::GlobalOptions;
use oxidized::global_options::NoneOrAllExcept;
use oxidized::global_options::SavedState;
use oxidized::global_options::SavedStateLoading;
use oxidized::hh_config::HhConfig as OxidizedHhConfig;
use oxidized::parser_options::ParserOptions;
use oxidized::saved_state_rollouts::SavedStateRollouts;
use packages::PackageInfo;
use serde_json::json;
use sha1::Digest;
use sha1::Sha1;

pub const FILE_PATH_RELATIVE_TO_ROOT: &str = ".hhconfig";
const PACKAGE_FILE_PATH_RELATIVE_TO_ROOT: &str = "PACKAGES.toml";
/// Must match `Config_keys.Hhconfig.enable_implicit_packages` on the OCaml side.
const ENABLE_IMPLICIT_PACKAGES_KEY: &str = "enable_implicit_packages";

fn get_optional_tristate(config: &ConfigFile, key: &str) -> Result<Option<isize>> {
    let Some(value) = config.get_int(key) else {
        return Ok(None);
    };
    let value = value?;
    anyhow::ensure!((0..=2).contains(&value), "{key} must be 0, 1, or 2");
    Ok(Some(value))
}

fn get_tristate(config: &ConfigFile, key: &str, default: isize) -> Result<isize> {
    Ok(get_optional_tristate(config, key)?.unwrap_or(default))
}

#[derive(Debug, Clone, Default)]
pub struct DistcConfig {
    pub min_worker_memory_gib: i64,
    pub min_cpu_units: i64,
    pub decl_buckets: i64,
    pub enable_p2p: bool,
    pub upload_only_changed_files: bool,
}

/// Aggregated configuration consumed by Rust Hack tools.
#[derive(Debug, Clone)]
pub struct HhConfig {
    pub common: OxidizedHhConfig,
    pub distc: DistcConfig,
}

impl Default for HhConfig {
    fn default() -> Self {
        Self {
            common: OxidizedHhConfig {
                version: None,
                ignored_paths: Vec::new(),
                hash: String::new(),
                opts: GlobalOptions::default(),
                gc_minor_heap_size: 0,
                gc_space_overhead: 0,
                sharedmem_global_size: 0,
                sharedmem_hash_table_pow: 0,
                sharedmem_heap_size: 0,
                ide_fall_back_to_full_index: false,
                hh_distc_should_disable_trace_store: false,
                hh_distc_exponential_backoff_num_retries: 10,
                naming_table_compression_level: 0,
                naming_table_compression_threads: 0,
                eden_fetch_parallelism: 0,
                use_distc_crawl_dircache: false,
            },
            distc: DistcConfig::default(),
        }
    }
}

impl HhConfig {
    pub fn from_root(root: impl AsRef<Path>, overrides: &ConfigFile) -> Result<Self> {
        let hhconfig_path = root.as_ref().join(FILE_PATH_RELATIVE_TO_ROOT);
        let hh_conf_path = system_config_path();
        Self::from_files(root, hhconfig_path, hh_conf_path, overrides)
    }

    fn create_custom_errors_path(hhconfig_path: &Path) -> PathBuf {
        // Unwrap is safe because hhconfig_path is always at least one nonempty string
        let mut packages_path = hhconfig_path.parent().unwrap().to_path_buf();
        packages_path.push("CUSTOM_ERRORS.json");
        packages_path
    }

    fn get_repo_packages_config_path<'a>(config: &'a ConfigFile, default: &'a str) -> &'a str {
        config.get_str("packages_config_path").unwrap_or(default)
    }

    /// Get package info from hhconfig with overrides already applied
    fn get_package_info(root: impl AsRef<Path>, hhconfig: &ConfigFile) -> PackageInfo {
        let package_config_pathbuf =
            Self::get_repo_packages_config_path(hhconfig, PACKAGE_FILE_PATH_RELATIVE_TO_ROOT);
        // Gate implicit packages on the same .hhconfig flag the OCaml typechecker
        // reads, so the Rust-side package info stays consistent with it.
        let enable_implicit_packages = hhconfig
            .get_bool_or(ENABLE_IMPLICIT_PACKAGES_KEY, false)
            .unwrap_or(false);
        PackageInfo::from_text_non_strict(
            enable_implicit_packages,
            root.as_ref()
                .join(package_config_pathbuf)
                .to_str()
                .unwrap_or_default(),
        )
        .unwrap_or_default()
    }

    fn from_files(
        root: impl AsRef<Path>,
        hhconfig_path: impl AsRef<Path>,
        hh_conf_path: impl AsRef<Path>,
        overrides: &ConfigFile,
    ) -> Result<Self> {
        let hhconfig_path = hhconfig_path.as_ref();
        let custom_error_config_path = Self::create_custom_errors_path(hhconfig_path);
        let (contents, mut hhconfig) = ConfigFile::from_file_with_contents(hhconfig_path)
            .with_context(|| hhconfig_path.display().to_string())?;
        let hash = Self::hash_from_files(&hhconfig, contents, custom_error_config_path.as_path())?;
        hhconfig.apply_overrides(overrides);
        let hh_conf_path = hh_conf_path.as_ref();

        // We don't try to gracefully handle the case where the /etc/hh.conf
        // (or overridden path) does not exist on disk.
        //
        // There is no benefit: if someone makes a mistake with machine
        // configurations, we rather have everything blow up in our face fast
        // and tell us plainly what is going wrong than doing something "intelligent"
        // and falling back to some default configuration, which is bound to lead to
        // symptoms that are difficult to debug.
        let mut hh_conf = ConfigFile::from_file(hh_conf_path)
            .with_context(|| hh_conf_path.display().to_string())?;

        hh_conf.apply_overrides(overrides);
        let custom_error_config =
            CustomErrorConfig::from_path(&custom_error_config_path).unwrap_or_default();
        let mut config = Self::from_configs(root, hhconfig, hh_conf, custom_error_config)?;
        config.common.hash = hash;
        Ok(config)
    }

    /// Returns all config files, as well as hhconfig hash.
    /// The hash is calculated as the sha1 hash of the concatenated
    /// contents of the config files (hhconfig, package config, custom errors config)
    pub fn into_config_files_with_hash(
        root: impl AsRef<Path>,
    ) -> Result<((ConfigFile, ConfigFile, CustomErrorConfig), String)> {
        let hhconfig_path = root.as_ref().join(FILE_PATH_RELATIVE_TO_ROOT);
        let hh_conf_path = system_config_path();
        let custom_error_config_pathbuf = Self::create_custom_errors_path(hhconfig_path.as_path());
        let custom_error_config_path = custom_error_config_pathbuf.as_path();
        let (hhconfig_contents, hh_config_file) =
            ConfigFile::from_file_with_contents(&hhconfig_path)
                .with_context(|| hhconfig_path.display().to_string())?;
        let hash =
            Self::hash_from_files(&hh_config_file, hhconfig_contents, custom_error_config_path)?;
        let hh_conf_file = ConfigFile::from_file(&hh_conf_path)
            .with_context(|| hh_conf_path.display().to_string())?;
        let custom_error_config = CustomErrorConfig::from_path(custom_error_config_path)?;
        Ok(((hh_conf_file, hh_config_file, custom_error_config), hash))
    }

    fn hash_from_files(
        hhconfig: &ConfigFile,
        hhconfig_contents: String,
        custom_error_config_path: &Path,
    ) -> Result<String> {
        // Grab extra config and use it to process the hash
        let package_config_path_from_hhconfig =
            Self::get_repo_packages_config_path(hhconfig, PACKAGE_FILE_PATH_RELATIVE_TO_ROOT);
        let package_config_path: PathBuf =
            Self::get_repo_packages_config_path(hhconfig, package_config_path_from_hhconfig).into();
        let package_contents: String = if package_config_path.exists() {
            let ctxt = || package_config_path.display().to_string();
            let bytes = std::fs::read(&package_config_path).with_context(ctxt)?;
            String::from_utf8(bytes).unwrap()
        } else {
            String::new()
        };
        let custom_error_contents: String = if custom_error_config_path.exists() {
            let ctxt = || custom_error_config_path.display().to_string();
            let bytes = std::fs::read(custom_error_config_path).with_context(ctxt)?;
            String::from_utf8(bytes).unwrap()
        } else {
            "[]".to_string()
        };
        Ok(Self::hash(
            hhconfig,
            &hhconfig_contents,
            &package_contents,
            &custom_error_contents,
        ))
    }

    fn hash(
        parsed: &ConfigFile,
        config_contents: &str,
        package_config: &str,
        custom_error_config: &str,
    ) -> String {
        if let Some(hash) = parsed.get_str("override_hhconfig_hash") {
            return hash.to_owned();
        }
        let mut hasher = Sha1::new();
        hasher.update(config_contents.as_bytes());
        hasher.update(package_config.as_bytes());
        hasher.update(custom_error_config.as_bytes());
        format!("{:x}", hasher.finalize())
    }

    /// Construct from .hhconfig and hh.conf files with CLI overrides already applied.
    pub fn from_configs(
        root: impl AsRef<Path>,
        hhconfig: ConfigFile,
        hh_conf: ConfigFile,
        custom_error_config: CustomErrorConfig,
    ) -> Result<Self> {
        let current_rolled_out_flag_idx = hhconfig
            .get_int("current_saved_state_rollout_flag_index")
            .unwrap_or(Ok(0))?;
        let deactivate_saved_state_rollout = hhconfig
            .get_bool("deactivate_saved_state_rollout")
            .unwrap_or(Ok(false))?;

        let version = hhconfig.get_str("version");
        let package_info: PackageInfo = Self::get_package_info(root, &hhconfig);
        let default = ParserOptions::default();
        let experimental_features = hhconfig.get_str("enable_experimental_stx_features");
        let po = ParserOptions {
            hhvm_compat_mode: default.hhvm_compat_mode,
            hhi_mode: default.hhi_mode,
            auto_namespace_map: match hhconfig.get_str("auto_namespace_map") {
                None => default.auto_namespace_map,
                Some(s) => parse_json::<BTreeMap<String, String>>(s)?
                    .into_iter()
                    .collect(),
            },
            codegen: hhconfig.get_bool_or("codegen", default.codegen)?,
            deregister_php_stdlib: hhconfig
                .get_bool_or("deregister_php_stdlib", default.deregister_php_stdlib)?,
            allow_unstable_features: hh_conf
                .bool_if_min_version("allow_unstable_features", version)
                .unwrap_or(Ok(default.allow_unstable_features))?,
            union_intersection_type_hints: hhconfig.get_bool_or(
                "union_intersection_type_hints",
                default.union_intersection_type_hints,
            )?,
            allowed_decl_fixme_codes: hhconfig
                .get_int_set_or("allowed_decl_fixme_codes", default.allowed_decl_fixme_codes)?,
            const_static_props: default.const_static_props,
            abstract_static_props: default.abstract_static_props,
            enable_xhp_class_modifier: hhconfig.get_bool_or(
                "enable_xhp_class_modifier",
                default.enable_xhp_class_modifier,
            )?,
            disable_xhp_element_mangling: hhconfig.get_bool_or(
                "disable_xhp_element_mangling",
                default.disable_xhp_element_mangling,
            )?,
            disable_xhp_children_declarations: hhconfig.get_bool_or(
                "disable_xhp_children_declarations",
                default.disable_xhp_children_declarations,
            )?,
            keep_user_attributes: default.keep_user_attributes,
            is_systemlib: default.is_systemlib,
            interpret_soft_types_as_like_types: hhconfig.get_bool_or(
                "interpret_soft_types_as_like_types",
                default.interpret_soft_types_as_like_types,
            )?,
            no_parser_readonly_check: default.no_parser_readonly_check,
            everything_sdt: hhconfig.get_bool_or("everything_sdt", default.everything_sdt)?,
            include_enum_member_values: hhconfig.get_bool_or(
                "include_enum_member_values",
                default.include_enum_member_values,
            )?,
            include_const_shape_key_values: hhconfig.get_bool_or(
                "include_const_shape_key_values",
                default.include_const_shape_key_values,
            )?,
            unwrap_concurrent: default.unwrap_concurrent,
            stack_size: default.stack_size,
            use_legacy_experimental_feature_config: false,
            experimental_features: experimental_features.map_or(
                Ok::<_, anyhow::Error>(default.experimental_features),
                parse_experimental_features,
            )?,
            // If there was no experimental features status list in configuration, consider all
            // existing experimental features to be released and hence usable.
            consider_unspecified_experimental_features_released: experimental_features.is_none(),
            // If there are errors, ignore them for the tcopt, the parser errors will be caught and
            // sent separately.
            package_info: package_info.try_into().unwrap_or_default(),
            package_support_multifile_tests: hhconfig.get_bool_or(
                "package_support_multifile_tests",
                default.package_support_multifile_tests,
            )?,
            disallow_non_annotated_memoize: hhconfig.get_bool_or(
                "disallow_non_annotated_memoize",
                default.disallow_non_annotated_memoize,
            )?,
            treat_non_annotated_memoize_as_kbic: hhconfig.get_bool_or(
                "treat_non_annotated_memoize_as_kbic",
                default.treat_non_annotated_memoize_as_kbic,
            )?,
            ignore_string_methods: hhconfig
                .get_bool_or("ignore_string_methods", default.ignore_string_methods)?,
            enable_intrinsics_extension: default.enable_intrinsics_extension,
            expression_tree_shape_no_unwrap: hhconfig.get_bool_or(
                "expression_tree_shape_no_unwrap",
                default.expression_tree_shape_no_unwrap,
            )?,
            disallow_late_bound_defaults: hhconfig.get_bool_or(
                "disallow_late_bound_defaults",
                default.disallow_late_bound_defaults,
            )?,
        };
        let rollouts = SavedStateRollouts::make(
            current_rolled_out_flag_idx,
            deactivate_saved_state_rollout,
            hh_conf.get_str("ss_force"),
            |flag_name| hh_conf.get_bool(flag_name).unwrap_or(Ok(false)),
        )?;
        let default = GlobalOptions::default();
        let opts = GlobalOptions {
            po,
            tco_saved_state: SavedState {
                loading: SavedStateLoading::default(),
                rollouts,
                project_metadata_w_flags: hh_conf
                    .get_bool("project_metadata_w_flags")
                    .unwrap_or(Ok(default.tco_saved_state.project_metadata_w_flags))?,
            },
            tco_legacy_experimental_features: hhconfig.get_string_set_or(
                "enable_experimental_tc_features",
                default.tco_legacy_experimental_features,
            ),
            tco_migration_flags: default.tco_migration_flags,
            tco_num_local_workers: default.tco_num_local_workers,
            tco_locl_cache_capacity: hhconfig
                .get_int_or("locl_cache_capacity", default.tco_locl_cache_capacity)?,
            tco_locl_cache_node_threshold: hhconfig.get_int_or(
                "locl_cache_node_threshold",
                default.tco_locl_cache_node_threshold,
            )?,
            so_naming_sqlite_path: default.so_naming_sqlite_path,
            po_disallow_toplevel_requires: hhconfig.get_bool_or(
                "disallow_toplevel_requires",
                default.po_disallow_toplevel_requires,
            )?,
            tco_log_large_fanouts_threshold: default.tco_log_large_fanouts_threshold,
            tco_log_inference_constraints: default.tco_log_inference_constraints,
            tco_language_feature_logging: default.tco_language_feature_logging,
            tco_timeout: hhconfig.get_int_or("timeout", default.tco_timeout)?,
            tco_constraint_array_index_assign: hhconfig.get_bool_or(
                "constraint_array_index_assign",
                default.tco_constraint_array_index_assign,
            )?,
            tco_constraint_method_call: hhconfig
                .get_bool_or("constraint_method_call", default.tco_constraint_method_call)?,
            code_agnostic_fixme: hhconfig
                .get_bool_or("code_agnostic_fixme", default.code_agnostic_fixme)?,
            allowed_fixme_codes_strict: hhconfig.get_int_set_or(
                "allowed_fixme_codes_strict",
                default.allowed_fixme_codes_strict,
            )?,
            log_levels: hhconfig
                .get_str("log_levels")
                .map_or(Ok(default.log_levels), parse_json)?,
            tco_fetch_remote_old_decls: default.tco_fetch_remote_old_decls,
            tco_only_fetch_remote_old_decl_during_init: default
                .tco_only_fetch_remote_old_decl_during_init,
            tco_disable_rust_provider_shallow_decl_invalidation: default
                .tco_disable_rust_provider_shallow_decl_invalidation,
            tco_enable_annotation_agnostic_decl_diffing: default
                .tco_enable_annotation_agnostic_decl_diffing,
            tco_populate_member_heaps: default.tco_populate_member_heaps,
            tco_skip_hierarchy_checks: default.tco_skip_hierarchy_checks,
            tco_silence_errors_under_dynamic: hhconfig.get_bool_or(
                "silence_errors_under_dynamic",
                default.tco_silence_errors_under_dynamic,
            )?,
            tco_reject_promoted_property_redeclaration: hhconfig.get_bool_or(
                "reject_promoted_property_redeclaration",
                default.tco_reject_promoted_property_redeclaration,
            )?,
            tco_skip_tast_checks: default.tco_skip_tast_checks,
            tco_strict_contexts: default.tco_strict_contexts,
            tco_like_casts: hhconfig.get_bool_or("like_casts", default.tco_like_casts)?,

            tco_disallow_unresolved_type_variables: default.tco_disallow_unresolved_type_variables,
            tco_custom_error_config: custom_error_config,
            tco_const_attribute: default.tco_const_attribute,
            glean_reponame: default.glean_reponame,
            symbol_write_index_inherited_members: default.symbol_write_index_inherited_members,
            symbol_write_ownership: default.symbol_write_ownership,
            symbol_write_root_path: default.symbol_write_root_path,
            symbol_write_hhi_path: default.symbol_write_hhi_path,
            symbol_write_ignore_paths: default.symbol_write_ignore_paths,
            symbol_write_index_paths: default.symbol_write_index_paths,
            symbol_write_index_paths_file: default.symbol_write_index_paths_file,
            symbol_write_index_paths_file_output: default.symbol_write_index_paths_file_output,
            symbol_write_include_hhi: default.symbol_write_include_hhi,
            symbol_write_sym_hash_in: default.symbol_write_sym_hash_in,
            symbol_write_exclude_out: default.symbol_write_exclude_out,
            symbol_write_referenced_out: default.symbol_write_referenced_out,
            symbol_write_reindexed_out: default.symbol_write_reindexed_out,
            symbol_write_sym_hash_out: default.symbol_write_sym_hash_out,
            tco_disallow_discarded_nullable_awaitables: default
                .tco_disallow_discarded_nullable_awaitables,
            tco_check_duplicate_enum_values: hhconfig.get_bool_or(
                "check_duplicate_enum_values",
                default.tco_check_duplicate_enum_values,
            )?,
            tco_require_shape_key_attribute: hhconfig.get_bool_or(
                "require_shape_key_attribute",
                default.tco_require_shape_key_attribute,
            )?,
            tco_require_const_shape_key_be_resolvable: hhconfig.get_bool_or(
                "require_const_shape_key_be_resolvable",
                default.tco_require_const_shape_key_be_resolvable,
            )?,
            tco_typecheck_sample_rate: hhconfig
                .get_float_or("typecheck_sample_rate", default.tco_typecheck_sample_rate)?,
            tco_pessimise_builtins: hhconfig
                .get_bool_or("pessimise_builtins", default.tco_pessimise_builtins)?,
            tco_enable_no_auto_dynamic: hhconfig
                .get_bool_or("enable_no_auto_dynamic", default.tco_enable_no_auto_dynamic)?,
            tco_skip_check_under_dynamic: hhconfig.get_bool_or(
                "skip_check_under_dynamic",
                default.tco_skip_check_under_dynamic,
            )?,
            tco_typed_open_shapes: hhconfig
                .get_bool_or("typed_open_shapes", default.tco_typed_open_shapes)?,
            tco_named_variadic_type: hhconfig
                .get_bool_or("named_variadic_type", default.tco_named_variadic_type)?,
            tco_variadic_named_parameters: hhconfig.get_bool_or(
                "variadic_named_parameters",
                default.tco_variadic_named_parameters,
            )?,
            tco_global_access_check_enabled: hhconfig.get_bool_or(
                "tco_global_access_check_enabled",
                default.tco_global_access_check_enabled,
            )?,
            tco_enable_expression_trees: default.tco_enable_expression_trees,
            tco_allowed_expression_tree_visitors: hhconfig
                .get_str("allowed_expression_tree_visitors")
                .map_or(default.tco_allowed_expression_tree_visitors, |s| {
                    let mut allowed_expression_tree_visitors = parse_svec(s);
                    // Fix up type names so they will match with elaborated names.
                    // Keep this in sync with the Utils.add_ns loop in server/server_config.ml
                    for ty in &mut allowed_expression_tree_visitors {
                        if !ty.starts_with('\\') {
                            *ty = format!("\\{}", ty)
                        }
                    }
                    allowed_expression_tree_visitors
                }),
            tco_typeconst_concrete_concrete_error: default.tco_typeconst_concrete_concrete_error,
            tco_meth_caller_only_public_visibility: default.tco_meth_caller_only_public_visibility,
            tco_require_extends_implements_ancestors: default
                .tco_require_extends_implements_ancestors,
            tco_implicit_inherit_sdt: default.tco_implicit_inherit_sdt,
            tco_explicit_consistent_constructors: hhconfig.get_int_or(
                "explicit_consistent_constructors",
                default.tco_explicit_consistent_constructors,
            )?,
            tco_require_types_class_consts: hhconfig.get_int_or(
                "require_types_tco_require_types_class_consts",
                default.tco_require_types_class_consts,
            )?,
            tco_type_printer_fuel: hhconfig
                .get_int_or("type_printer_fuel", default.tco_type_printer_fuel)?,
            tco_specify_manifold_api_key: default.tco_specify_manifold_api_key,
            tco_profile_top_level_definitions: hhconfig.get_bool_or(
                "profile_top_level_definitions",
                default.tco_profile_top_level_definitions,
            )?,
            tco_typecheck_if_name_matches_regexp: default.tco_typecheck_if_name_matches_regexp,
            tco_allow_all_files_for_module_declarations: default
                .tco_allow_all_files_for_module_declarations,
            tco_allowed_files_for_module_declarations: hhconfig
                .get_str("allowed_files_for_module_declarations")
                .map_or(
                    default.tco_allowed_files_for_module_declarations,
                    parse_svec,
                ),
            tco_allowed_files_for_ignore_readonly: hhconfig
                .get_str("allowed_files_for_ignore_readonly")
                .map_or(default.tco_allowed_files_for_ignore_readonly, parse_svec),
            tco_record_fine_grained_dependencies: default.tco_record_fine_grained_dependencies,
            tco_loop_iteration_upper_bound: default.tco_loop_iteration_upper_bound,
            tco_populate_dead_unsafe_cast_heap: default.tco_populate_dead_unsafe_cast_heap,
            tco_dynamic_inference: default.tco_dynamic_inference,
            dump_tast_hashes: hhconfig
                .get_bool("dump_tast_hashes")
                .or_else(|| hh_conf.get_bool("dump_tast_hashes"))
                .unwrap_or(Ok(default.dump_tast_hashes))?,
            dump_tasts: match hh_conf.get_str("dump_tasts") {
                None => default.dump_tasts,
                Some(path) => {
                    let path = PathBuf::from(path);
                    let file =
                        File::open(&path).with_context(|| path.to_string_lossy().to_string())?;
                    BufReader::new(file)
                        .lines()
                        .collect::<std::io::Result<_>>()?
                }
            },
            tco_autocomplete_mode: default.tco_autocomplete_mode,
            tco_extended_reasons: hhconfig.get_either_int_or_str("extended_reasons").and_then(
                |res| match res {
                    Ok(n) => Some(ExtendedReasonsConfig::Extended(n)),
                    Err(data) => {
                        if data.eq("debug") {
                            Some(ExtendedReasonsConfig::Debug)
                        } else if data.eq("legacy") {
                            Some(ExtendedReasonsConfig::Legacy)
                        } else {
                            None
                        }
                    }
                },
            ),
            tco_disable_physical_equality: hh_conf.get_bool_or(
                "disable_physical_equality",
                default.tco_disable_physical_equality,
            )?,
            hack_warnings: {
                let is_on = hhconfig
                    .get_bool("hack_warnings")
                    .or_else(|| hh_conf.get_bool("hack_warnings"))
                    .unwrap_or(Ok(true))?;
                if is_on {
                    let disabled_warnings = hhconfig.get_ints_or("disabled_warnings", vec![])?;
                    NoneOrAllExcept::AllExcept(disabled_warnings)
                } else {
                    NoneOrAllExcept::NNone
                }
            },
            warnings_default_all: hhconfig
                .get_bool_or("warnings_default_all", default.warnings_default_all)?,
            warnings_in_sandcastle: hhconfig
                .get_bool_or("warnings_in_sandcastle", default.warnings_in_sandcastle)?,
            warnings_generated_files: hhconfig
                .get_str("warnings_generated_files")
                .map_or(default.warnings_generated_files, parse_svec),
            tco_package_allow_classconst_violations: hhconfig.get_bool_or(
                "package_allow_classconst_violations",
                default.tco_package_allow_classconst_violations,
            )?,
            tco_package_allow_all_tconst_violations: hhconfig.get_bool_or(
                "package_allow_all_tconst_violations",
                default.tco_package_allow_all_tconst_violations,
            )?,
            tco_package_allow_enforceable_enum_violations: hhconfig.get_bool_or(
                "package_allow_enforceable_enum_violations",
                default.tco_package_allow_enforceable_enum_violations,
            )?,
            tco_package_exclude_patterns: hhconfig
                .get_str("package_exclude_patterns")
                .map_or(default.tco_package_exclude_patterns, parse_svec),
            re_no_cache: hhconfig.get_bool_or("re_no_cache", default.re_no_cache)?,
            hh_distc_should_disable_trace_store: hhconfig.get_bool_or(
                "hh_distc_should_disable_trace_store",
                default.hh_distc_should_disable_trace_store,
            )?,
            hh_distc_exponential_backoff_num_retries: hhconfig.get_int_or(
                "hh_distc_exponential_backoff_num_retries",
                default.hh_distc_exponential_backoff_num_retries,
            )?,
            recursive_case_types: hhconfig
                .get_bool_or("recursive_case_types", default.recursive_case_types)?,
            tco_enabled_unstable_features: Default::default(),
            class_sub_classname: hhconfig
                .get_bool_or("class_sub_classname", default.class_sub_classname)?,
            class_class_type: hhconfig.get_bool_or("class_class_type", default.class_class_type)?,
            needs_concrete_class_call_check: get_tristate(
                &hhconfig,
                "needs_concrete_class_call_check",
                default.needs_concrete_class_call_check,
            )?,
            needs_concrete_class_function_pointer_check: get_optional_tristate(
                &hhconfig,
                "needs_concrete_class_function_pointer_check",
            )?,
            needs_concrete_require_extends_class_const_check: hhconfig.get_bool_or(
                "needs_concrete_require_extends_class_const_check",
                default.needs_concrete_require_extends_class_const_check,
            )?,
            strict_consistent_construct: hhconfig.get_bool_or(
                "strict_consistent_construct",
                default.strict_consistent_construct,
            )?,
            allow_class_string_cast: hhconfig
                .get_bool_or("allow_class_string_cast", default.allow_class_string_cast)?,
            class_pointer_ban_class_array_key: hhconfig.get_bool_or(
                "class_pointer_ban_class_array_key",
                default.class_pointer_ban_class_array_key,
            )?,
            tco_permits_bypassing_visibility: hhconfig
                .get_str_list("permits_bypassing_visibility")
                .map(|classes| classes.map(|s| format!("\\{}", s)).collect())
                .unwrap_or_default(),
            tco_tests_bypass_visibility_static_properties: hhconfig.get_bool_or(
                "tests_bypass_visibility_static_properties",
                default.tco_tests_bypass_visibility_static_properties,
            )?,
            tco_implicit_throw_points: hhconfig
                .get_bool_or("implicit_throw_points", default.tco_implicit_throw_points)?,
            tco_repo_stdlib_path: hhconfig.get_str("repo_stdlib_path").map(|p| p.to_string()),
        };
        let mut c = Self::default();
        c.common.opts = opts;

        for (key, mut value) in hhconfig {
            match key.as_str() {
                "version" => {
                    c.common.version = Some(value);
                }
                "ignored_paths" => {
                    c.common.ignored_paths = parse_json(&value)?;
                }
                "gc_minor_heap_size" => {
                    value.retain(|c| c != '_');
                    c.common.gc_minor_heap_size = parse_json(&value)?;
                }
                "gc_space_overhead" => {
                    c.common.gc_space_overhead = parse_json(&value)?;
                }
                "sharedmem_global_size" => {
                    value.retain(|c| c != '_');
                    c.common.sharedmem_global_size = parse_json(&value)?;
                }
                "sharedmem_hash_table_pow" => {
                    c.common.sharedmem_hash_table_pow = parse_json(&value)?;
                }
                "sharedmem_heap_size" => {
                    value.retain(|c| c != '_');
                    c.common.sharedmem_heap_size = parse_json(&value)?;
                }
                "ide_fall_back_to_full_index" => {
                    c.common.ide_fall_back_to_full_index = parse_json(&value)?;
                }
                "hh_distc_should_disable_trace_store" => {
                    c.common.hh_distc_should_disable_trace_store = parse_json(&value)?;
                }
                "naming_table_compression_threads" => {
                    c.common.naming_table_compression_threads = parse_json(&value)?;
                }
                "naming_table_compression_level" => {
                    c.common.naming_table_compression_level = parse_json(&value)?;
                }
                "eden_fetch_parallelism" => {
                    c.common.eden_fetch_parallelism = parse_json(&value)?;
                }
                "hh_distc_exponential_backoff_num_retries" => {
                    c.common.hh_distc_exponential_backoff_num_retries = parse_json(&value)?;
                }
                "use_distc_crawl_dircache" => {
                    c.common.use_distc_crawl_dircache = parse_json(&value)?;
                }
                "distc_min_worker_memory_gib" => {
                    c.distc.min_worker_memory_gib = parse_json(&value)?;
                }
                "distc_min_cpu_units" => {
                    c.distc.min_cpu_units = parse_json(&value)?;
                }
                "distc_decl_buckets" => {
                    c.distc.decl_buckets = parse_json(&value)?;
                }
                "distc_enable_p2p" => {
                    c.distc.enable_p2p = parse_json(&value)?;
                }
                "distc_upload_only_changed_files" => {
                    c.distc.upload_only_changed_files = parse_json(&value)?;
                }
                _ => {}
            }
        }
        Ok(c)
    }

    pub fn to_selected_experiments_json(&self) -> String {
        let experiments = json!({
            "eden_fetch_parallelism": self.common.eden_fetch_parallelism,
            "use_distc_crawl_dircache": self.common.use_distc_crawl_dircache,
            "distc_enable_p2p": self.distc.enable_p2p,
            "distc_upload_only_changed_files": self.distc.upload_only_changed_files,
        });
        experiments.to_string()
    }
}

fn parse_json<'de, T: serde::de::Deserialize<'de>>(value: &'de str) -> Result<T> {
    Ok(serde_json::from_slice(value.as_bytes())?)
}

fn parse_experimental_features(
    s: &str,
) -> Result<BTreeMap<String, experimental_features::FeatureStatus>> {
    let features = parse_json::<BTreeMap<String, String>>(s)?;
    features
        .into_iter()
        .map(experimental_features::FeatureName::parse_experimental_feature)
        .collect()
}

fn parse_svec(value: &str) -> Vec<String> {
    value
        .split_terminator(',')
        .map(|s| s.trim().to_owned())
        .collect()
}

/// Return the local config file path, allowing HH_LOCALCONF_PATH to override it.
fn system_config_path() -> PathBuf {
    const HH_CONF: &str = "hh.conf";
    match std::env::var_os("HH_LOCALCONF_PATH") {
        Some(path) => Path::new(&path).join(HH_CONF),
        None => Path::new("/etc").join(HH_CONF), // TODO see options/build_options.ml for mac cfg
    }
}

#[cfg(test)]
mod test {
    use super::*;
    fn from_slice(bytes: &[u8]) -> Result<HhConfig> {
        let (hash, config) = ConfigFile::from_slice_with_sha1(bytes);
        let mut hhconf = HhConfig::from_configs(
            PathBuf::new(),
            config,
            Default::default(),
            Default::default(),
        )?;
        hhconf.common.hash = hash;
        Ok(hhconf)
    }

    #[test]
    fn test_log_levels() {
        let hhconf = from_slice(br#"log_levels={ "pessimise": 1 }"#).unwrap();
        assert_eq!(
            hhconf.common.opts.log_levels.get("pessimise").copied(),
            Some(1isize)
        );
    }

    #[test]
    fn test_needs_concrete_fine_grained() {
        let hhconf = from_slice(b"needs_concrete_class_call_check=2").unwrap();
        assert_eq!(hhconf.common.opts.needs_concrete_class_call_check, 2);
        assert_eq!(
            hhconf
                .common
                .opts
                .needs_concrete_class_function_pointer_check,
            None
        );
    }

    #[test]
    fn test_needs_concrete_class_function_pointer_check() {
        let hhconf = from_slice(
            b"needs_concrete_class_call_check=2\nneeds_concrete_class_function_pointer_check=1",
        )
        .unwrap();
        assert_eq!(hhconf.common.opts.needs_concrete_class_call_check, 2);
        assert_eq!(
            hhconf
                .common
                .opts
                .needs_concrete_class_function_pointer_check,
            Some(1)
        );
        assert!(from_slice(b"needs_concrete_class_function_pointer_check=3").is_err());
    }
}
