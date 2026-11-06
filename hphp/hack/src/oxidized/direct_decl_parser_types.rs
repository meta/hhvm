// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated <<SignedSource::*O*zOeWoEQle#+L!plEphiEmie@IsG>>
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
    Default,
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
    ToOcamlRep,
)]
#[rust_to_ocaml(attr = "deriving show")]
#[repr(C)]
pub struct Decls(pub Vec<(String, shallow_decl_defs::Decl)>);

/// WARNING: This type has not undergone the processing that you often expect in the typechecker,
/// e.g. deregistering PHPStdLib symbols, or putting into forward lexical order, or removing
/// duplicates. It's often a good idea to use ParsedFileWithHashes instead (or rather, one of
/// its wrappers).
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
    ToOcamlRep,
)]
#[rust_to_ocaml(prefix = "pf_")]
#[repr(C)]
pub struct ParsedFile {
    /// Was the file Hack or HHI?
    pub mode: Option<file_info::Mode>,
    pub file_attributes: Vec<typing_defs::UserAttribute>,
    /// User attributes on top-level functions, keyed by elaborated name.
    pub function_attributes: Vec<(String, Vec<typing_defs::UserAttribute>)>,
    pub decls: Decls,
    /// How did the parser handle xhp elements? This is a copy of
    /// the setting from DeclParserOptions, for use when converting
    /// Decls to Facts.
    pub disable_xhp_element_mangling: bool,
    /// True if the FFP detected parse errors while parsing. Other parse errors
    /// are detected in a second pass over the CST, and this field does not
    /// indicate whether errors would be detected in that second pass.
    pub has_first_pass_parse_errors: bool,
    /// The module this file is a part of, if any.
    pub module_membership: Option<String>,
    /// The package this file is part of.
    pub package_membership: Option<String>,
}
