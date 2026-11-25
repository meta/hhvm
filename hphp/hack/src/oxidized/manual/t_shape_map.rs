// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::cmp::Ordering;
use std::hash::Hash;
use std::hash::Hasher;

use eq_modulo_pos::EqModuloPos;
use no_pos_hash::NoPosHash;
use ocamlrep::FromOcamlRep;
use ocamlrep::ToOcamlRep;
use serde::Deserialize;
use serde::Serialize;

use crate::typing_defs_core::PosByteString;
use crate::typing_defs_core::PosString;
use crate::typing_defs_core::TshapeFieldName;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[derive(FromOcamlRep, EqModuloPos, NoPosHash, ToOcamlRep)]
pub struct TShapeField(pub TshapeFieldName);

impl TShapeField {
    /// A symbolic key as emitted by the declaration parser.
    pub fn new(declared: TshapeFieldName) -> Self {
        Self(declared)
    }
}

/// Ord implementation that ignores pos
impl Ord for TShapeField {
    fn cmp(&self, other: &Self) -> Ordering {
        use TshapeFieldName::*;
        match (&self.0, &other.0) {
            (TSFregexGroup(PosString(_, s1)), TSFregexGroup(PosString(_, s2))) => s1.cmp(s2),
            (TSFlitStr(PosByteString(_, s1)), TSFlitStr(PosByteString(_, s2))) => s1.cmp(s2),
            (
                TSFclassConst((_, c1), PosString(_, m1)),
                TSFclassConst((_, c2), PosString(_, m2)),
            ) => (c1, m1).cmp(&(c2, m2)),
            (TSFregexGroup(_), _) => Ordering::Less,
            (TSFlitStr(_), TSFregexGroup(_)) => Ordering::Greater,
            (TSFlitStr(_), _) => Ordering::Less,
            (TSFclassConst(_, _), _) => Ordering::Greater,
        }
    }
}

impl PartialOrd for TShapeField {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for TShapeField {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}

impl Eq for TShapeField {}

// non-derived impl Hash because PartialEq and Eq are non-derived
impl Hash for TShapeField {
    fn hash<H: Hasher>(&self, hasher: &mut H) {
        Hash::hash(&self.0, hasher)
    }
}

pub type TShapeMap<T> = std::collections::BTreeMap<TShapeField, T>;
