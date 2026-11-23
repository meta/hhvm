// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use parser_core_types::parser_env::ParserEnv;

use crate::decl_parser_options::DeclParserOptions;
use crate::parser_options::ParserOptions;

impl Default for DeclParserOptions {
    fn default() -> Self {
        Self::from_parser_options(&ParserOptions::default())
    }
}

impl DeclParserOptions {
    pub fn from_parser_options(opts: &ParserOptions) -> Self {
        Self {
            auto_namespace_map: opts.auto_namespace_map.clone(),
            disable_xhp_element_mangling: opts.disable_xhp_element_mangling,
            interpret_soft_types_as_like_types: opts.interpret_soft_types_as_like_types,
            enable_xhp_class_modifier: opts.enable_xhp_class_modifier,
            everything_sdt: opts.everything_sdt,
            php5_compat_mode: false,
            hhvm_compat_mode: false,
            keep_user_attributes: false,
            include_assignment_values: false,
            stack_size: opts.stack_size,
            deregister_php_stdlib: opts.deregister_php_stdlib,
            package_info: opts.package_info.clone(),
            package_support_multifile_tests: opts.package_support_multifile_tests,
            disallow_non_annotated_memoize: opts.disallow_non_annotated_memoize,
            treat_non_annotated_memoize_as_kbic: opts.treat_non_annotated_memoize_as_kbic,
            ignore_string_methods: opts.ignore_string_methods,
        }
    }
}

impl From<&DeclParserOptions> for ParserEnv {
    fn from(opts: &DeclParserOptions) -> Self {
        Self {
            hhvm_compat_mode: opts.hhvm_compat_mode,
            php5_compat_mode: opts.php5_compat_mode,
            disable_xhp_element_mangling: opts.disable_xhp_element_mangling,
            interpret_soft_types_as_like_types: opts.interpret_soft_types_as_like_types,
            enable_xhp_class_modifier: opts.enable_xhp_class_modifier,
            ..Default::default()
        }
    }
}

#[cfg(test)]
mod test {
    #[test]
    fn test_default_opts() {
        // Make sure ParserOptions and DeclParserOptions defaults don't drift
        // even if someone hand-edits or auto-derives Default for DeclParserOptions.
        assert_eq!(
            super::DeclParserOptions::default(),
            super::DeclParserOptions::from_parser_options(&Default::default())
        );
    }
}
