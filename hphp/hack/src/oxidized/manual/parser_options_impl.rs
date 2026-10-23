// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use crate::r#gen::parser_options::ParserOptions;
use crate::i_set;
use crate::package_info::PackageInfo;

impl Default for ParserOptions {
    fn default() -> Self {
        Self {
            const_static_props: false,
            abstract_static_props: false,
            enable_xhp_class_modifier: false,
            disable_xhp_element_mangling: false,
            allow_unstable_features: false, // true in /etc/hh.conf
            hhvm_compat_mode: false,
            hhi_mode: false,
            codegen: false,
            disable_xhp_children_declarations: false,
            interpret_soft_types_as_like_types: false,
            is_systemlib: false,
            auto_namespace_map: vec![],
            everything_sdt: false,
            include_enum_member_values: false,
            include_const_shape_key_values: false,
            deregister_php_stdlib: false,
            stack_size: 32 * 1024 * 1024, // 32 MiB is the largest stack size we can use without requiring sudo
            keep_user_attributes: false,
            union_intersection_type_hints: false,
            unwrap_concurrent: false,
            no_parser_readonly_check: false,
            allowed_decl_fixme_codes: i_set::ISet::new(),
            use_legacy_experimental_feature_config: true,
            experimental_features: std::collections::BTreeMap::default(),
            consider_unspecified_experimental_features_released: true,
            package_info: PackageInfo::default(),
            package_support_multifile_tests: false,
            disallow_non_annotated_memoize: false,
            treat_non_annotated_memoize_as_kbic: false,
            ignore_string_methods: true,
            enable_intrinsics_extension: false,
            expression_tree_shape_no_unwrap: false,
            disallow_late_bound_defaults: false,
        }
    }
}
