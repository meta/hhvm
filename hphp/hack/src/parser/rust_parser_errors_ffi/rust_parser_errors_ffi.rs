// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::HashSet;

use bumpalo::Bump;
use ocamlrep_ocamlpool::ocaml_ffi;
use oxidized::namespace_env::Mode;
use oxidized::parser_options::FfiT;
use oxidized::parser_options::ParserOptions;
use parser_core_types::source_text::SourceText;
use parser_core_types::syntax_by_ref::positioned_syntax::PositionedSyntax;
use parser_core_types::syntax_error::SyntaxError;
use parser_core_types::syntax_tree::SyntaxTree;

ocaml_ffi! {
    fn drop_tree_positioned(ocaml_tree: usize) {
        unsafe {
            let pair = Box::from_raw(ocaml_tree as *mut (usize, usize));
            let _ = Box::from_raw(pair.0 as *mut SyntaxTree<'_, PositionedSyntax<'_>, ()>);
            let _ = Box::from_raw(pair.1 as *mut Bump);
        }
    }

    fn rust_parser_errors_positioned(
        source_text: SourceText<'_>,
        ocaml_tree: usize,
        options: FfiT,
    ) -> Vec<SyntaxError> {
        let FfiT {
            hhvm_compat_mode,
            hhi_mode,
            codegen,
            const_static_props,
            abstract_static_props,
            enable_xhp_class_modifier,
            disable_xhp_element_mangling,
            disable_xhp_children_declarations,
            allow_unstable_features,
            interpret_soft_types_as_like_types,
            is_systemlib,
            use_legacy_experimental_feature_config,
            experimental_features,
            consider_unspecified_experimental_features_released,
            ignore_string_methods,
            enable_intrinsics_extension,
            allow_single_statement_concurrent,
        } = options;
        // Only the options relevant to parser errors are populated; the rest retain their defaults.
        let parser_options = ParserOptions {
            const_static_props,
            abstract_static_props,
            enable_xhp_class_modifier,
            disable_xhp_element_mangling,
            disable_xhp_children_declarations,
            allow_unstable_features,
            interpret_soft_types_as_like_types,
            is_systemlib,
            use_legacy_experimental_feature_config,
            experimental_features,
            consider_unspecified_experimental_features_released,
            ignore_string_methods,
            enable_intrinsics_extension,
            allow_single_statement_concurrent,
            ..ParserOptions::default()
        };
        let (tree, _arena) = unsafe {
            // A rust pointer of (&SyntaxTree, &Arena) is passed to Ocaml in rust_parser_ffi::parse,
            // Ocaml passes it back here
            // PLEASE ENSURE TYPE SAFETY MANUALLY!!!
            let pair = Box::from_raw(ocaml_tree as *mut (usize, usize));
            let tree = <SyntaxTree<'_, PositionedSyntax<'_>, ()>>::ffi_pointer_into_boxed(
                pair.0,
                &source_text,
            );
            let arena = Box::from_raw(pair.1 as *mut Bump);
            (tree, arena)
        };

        let (errors, _, _) = rust_parser_errors::parse_errors(
            &tree,
            parser_options,
            hhvm_compat_mode,
            hhi_mode,
            if codegen {
                Mode::ForCodegen
            } else {
                Mode::ForTypecheck
            },
            is_systemlib,
            HashSet::default(),
        );
        errors
    }
}
