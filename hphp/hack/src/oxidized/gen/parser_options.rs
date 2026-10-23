// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<1446f2a8483c104e984a753fde528a74>>
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
pub struct ParserOptions {
    /// Enable const static properties
    pub const_static_props: bool,
    /// Static properties can be abstract
    pub abstract_static_props: bool,
    /// Enable the new style xhp class.
    /// Old style: class :name {}
    /// New style: xhp class name {}
    pub enable_xhp_class_modifier: bool,
    /// Flag to disable the old stype xhp element mangling. `<something/>` would otherwise be resolved as `xhp_something`
    /// The new style `xhp class something {}` does not do this style of mangling, thus we need a way to disable it on the
    /// 'lookup side'.
    pub disable_xhp_element_mangling: bool,
    /// Allows enabling unstable features via the __EnableUnstableFeatures attribute
    pub allow_unstable_features: bool,
    pub hhvm_compat_mode: bool,
    pub hhi_mode: bool,
    /// Are we emitting bytecode?
    pub codegen: bool,
    /// Disable `children (foo|bar+|pcdata)` declarations as they can be implemented without special syntax
    pub disable_xhp_children_declarations: bool,
    /// <<__Soft>> T -> ~T
    pub interpret_soft_types_as_like_types: bool,
    /// Enable features used to typecheck systemlib
    pub is_systemlib: bool,
    /// Namespace aliasing map
    pub auto_namespace_map: Vec<(String, String)>,
    /// All classes are implcitly marked <<__SupportDynamicType>>
    pub everything_sdt: bool,
    /// If set, the parser records enum member values in decls (used by the
    /// duplicate-enum-value check).
    pub include_enum_member_values: bool,
    /// If set, the parser records `__ShapeKey` class-constant metadata in
    /// decls.
    pub include_const_shape_key_values: bool,
    /// Parse all user attributes rather than only the ones needed for typing
    pub keep_user_attributes: bool,
    /// Stack size to use for parallel workers inside the parser.
    pub stack_size: isize,
    /// Flag for disabling functions in HHI files with the __PHPStdLib attribute
    pub deregister_php_stdlib: bool,
    /// Enables union and intersection type hints
    pub union_intersection_type_hints: bool,
    /// Replace concurrent blocks with their bodies in the AST
    pub unwrap_concurrent: bool,
    /// Disable parser-based readonly checking
    pub no_parser_readonly_check: bool,
    /// Set of error codes disallowed in decl positions
    pub allowed_decl_fixme_codes: i_set::ISet,
    /// Ignore the experimental_features and consider_unspecified_experimental_features_released config
    /// options and use a hard coded function instead
    pub use_legacy_experimental_feature_config: bool,
    /// A mapping of names of experimental features to their status: Unstable/Preview/OngoingRelease
    pub experimental_features: s_map::SMap<experimental_features::FeatureStatus>,
    /// Any experimental features not specified in the experimental_features configuration field should
    /// default to OngoingRelease if this is true. Otherwise they will default to Unstable. This should be true for
    /// testing and tools that don't read .hhconfig (e.g., like hh_single_type_check and hh_parse). It should
    /// be false for hh_server.
    pub consider_unspecified_experimental_features_released: bool,
    /// Information used to determine which package a file belongs to during typechecking.
    pub package_info: package_info::PackageInfo,
    /// Option to strip the multifile filename mangling used in Hack tests.  Used in Packages tests.
    /// Should be set to true only by the unit tests in the Hack test suite
    pub package_support_multifile_tests: bool,
    /// When true, plain <<__Memoize>> will not be allowed.
    pub disallow_non_annotated_memoize: bool,
    /// When true, plain <<__Memoize>> will be treated as <<__Memoize(#KeyedByIC)>>.
    pub treat_non_annotated_memoize_as_kbic: bool,
    /// Ignore the methods defined in the string class in string.hhi
    pub ignore_string_methods: bool,
    pub enable_intrinsics_extension: bool,
    /// When true, the Expression Tree desugarer stops emitting `->__unwrap()` on the first
    /// argument of the shape ops (`shapeAt`/`shapeIdx`/`shapePut`). Used to roll out making the
    /// DSL's shape wrapper type a transparent type alias, after which the unwrap call no longer
    /// typechecks.
    pub expression_tree_shape_no_unwrap: bool,
    /// Disallow late-bound class references in default values: `static` in parameter and XHP
    /// attribute defaults, and `self` or `parent` in a trait's parameter defaults.
    pub disallow_late_bound_defaults: bool,
}

/// Options passed to the Rust parser-error checker. Oxidised into FfiT.
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
#[repr(C)]
pub struct FfiT {
    pub hhvm_compat_mode: bool,
    pub hhi_mode: bool,
    pub codegen: bool,
    pub const_static_props: bool,
    pub abstract_static_props: bool,
    pub enable_xhp_class_modifier: bool,
    pub disable_xhp_element_mangling: bool,
    pub disable_xhp_children_declarations: bool,
    pub allow_unstable_features: bool,
    pub interpret_soft_types_as_like_types: bool,
    pub is_systemlib: bool,
    pub use_legacy_experimental_feature_config: bool,
    pub experimental_features: s_map::SMap<experimental_features::FeatureStatus>,
    pub consider_unspecified_experimental_features_released: bool,
    pub ignore_string_methods: bool,
    pub enable_intrinsics_extension: bool,
}
