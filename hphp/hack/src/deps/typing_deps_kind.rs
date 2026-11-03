// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated <<SignedSource::*O*zOeWoEQle#+L!plEphiEmie@IsG>>
//
// To regenerate this file, run:
//   buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/src:oxidized_regen
use arena_trait::TrivialDrop;
use eq_modulo_pos::EqModuloPos;
use no_pos_hash::NoPosHash;
use ocamlrep::FromOcamlRep;
use ocamlrep::FromOcamlRepIn;
use ocamlrep::ToOcamlRep;
use serde::Deserialize;
use serde::Serialize;
use strum::EnumString;
use strum::FromRepr;

#[allow(unused_imports)]
use crate::*;

/// Dependency kinds and their stable hash tags. Tag 4 is reserved.
/// Constructor order also determines the OCaml representation.
#[derive(
    Clone,
    Copy,
    Debug,
    Deserialize,
    EnumString,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    FromOcamlRepIn,
    FromRepr,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep,
)]
#[rust_to_ocaml(attr = "deriving enum")]
#[repr(u8)]
pub enum DepKind {
    KGConst = 0,
    KFun = 1,
    KType = 2,
    KExtends = 3,
    KConst = 5,
    KConstructor = 6,
    KProp = 7,
    KSProp = 8,
    KMethod = 9,
    KSMethod = 10,
    KAllMembers = 11,
    KGConstName = 12,
    KModule = 13,
    KDeclares = 14,
    KNotSubtype = 15,
    KFile = 16,
}
impl TrivialDrop for DepKind {}
arena_deserializer::impl_deserialize_in_arena!(DepKind);
