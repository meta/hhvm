// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<1080ad2139066987c74353e4f8dcd83c>>
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
#[rust_to_ocaml(attr = "deriving show")]
#[repr(C)]
pub struct DeclParserOptions {
    pub auto_namespace_map: Vec<(String, String)>,
    pub disable_xhp_element_mangling: bool,
    pub interpret_soft_types_as_like_types: bool,
    pub enable_xhp_class_modifier: bool,
    pub everything_sdt: bool,
    pub include_const_shape_key_values: bool,
    pub php5_compat_mode: bool,
    pub hhvm_compat_mode: bool,
    pub keep_user_attributes: bool,
    pub include_assignment_values: bool,
    /// Stack size to for the parallel workers
    pub stack_size: isize,
    pub deregister_php_stdlib: bool,
    pub package_info: package_info::PackageInfo,
    pub package_support_multifile_tests: bool,
    pub disallow_non_annotated_memoize: bool,
    pub treat_non_annotated_memoize_as_kbic: bool,
    pub ignore_string_methods: bool,
}
