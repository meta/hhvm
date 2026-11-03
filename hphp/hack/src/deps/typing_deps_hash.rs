// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::hash::Hasher;

use fnv::FnvHasher;
use ocamlrep::FromOcamlRep;

/// Variant types used in the depgraph table.
///
/// NOTE: Keep in sync with the order of the fields in `Typing_deps.ml`.
#[derive(Copy, Clone, Debug, PartialEq, Eq, strum::EnumString, FromOcamlRep)]
#[repr(u8)]
pub enum DepType {
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

impl DepType {
    pub fn as_u8(self) -> u8 {
        self as u8
    }

    pub fn from_u8(tag: u8) -> Option<Self> {
        match tag {
            0 => Some(DepType::KGConst),
            1 => Some(DepType::KFun),
            2 => Some(DepType::KType),
            3 => Some(DepType::KExtends),
            5 => Some(DepType::KConst),
            6 => Some(DepType::KConstructor),
            7 => Some(DepType::KProp),
            8 => Some(DepType::KSProp),
            9 => Some(DepType::KMethod),
            10 => Some(DepType::KSMethod),
            11 => Some(DepType::KAllMembers),
            12 => Some(DepType::KGConstName),
            13 => Some(DepType::KModule),
            14 => Some(DepType::KDeclares),
            15 => Some(DepType::KNotSubtype),
            16 => Some(DepType::KFile),
            _ => None,
        }
    }
}

/// Select the hashing algorithm to use for dependency hashes.
///
/// FnvHasher appears to produce better hashes (fewer collisions) than
/// `std::collections::hash_map::DefaultHasher` on our workloads. However, other
/// hashing algorithms may perform better still.
fn make_hasher() -> FnvHasher {
    Default::default()
}

fn postprocess_hash(dep_type: DepType, hash: u64) -> u64 {
    let hash: u64 = match dep_type {
        DepType::KType => {
            // For class dependencies, set the lowest bit to 1. For extends
            // dependencies, the lowest bit will be 0 (in the case below), so we'll
            // be able to convert from a class hash to its extends hash without
            // reversing the hash.
            (hash << 1) | 1
        }
        _ => {
            // Ensure that only classes have the lowest bit set to 1, so that we
            // don't try to transitively traverse the subclasses of non-class
            // dependencies.
            hash << 1
        }
    };
    hash & !(1 << 63)
}

fn get_dep_type_hash_key(dep_type: DepType) -> u8 {
    match dep_type {
        DepType::KType | DepType::KExtends => {
            // Use the same tag for classes and extends dependencies, so that we can
            // convert between them without reversing the hash.
            DepType::KType as u8
        }
        _ => dep_type as u8,
    }
}

/// Hash a one-argument `Typing_deps.Dep.variant`'s fields.
pub fn hash1(dep_type: DepType, name1: &[u8]) -> u64 {
    let mut hasher = make_hasher();
    hasher.write_u8(get_dep_type_hash_key(dep_type));
    hasher.write(name1);
    postprocess_hash(dep_type, hasher.finish())
}

/// Hash a two-argument `Typing_deps.Dep.variant`'s fields.
pub fn hash2(dep_type: DepType, type_hash: u64, name2: &[u8]) -> u64 {
    let mut hasher = make_hasher();
    hasher.write_u8(get_dep_type_hash_key(dep_type));
    hasher.write_u64(type_hash);
    hasher.write(name2);
    postprocess_hash(dep_type, hasher.finish())
}

pub fn declares_hash() -> u64 {
    hash1(DepType::KDeclares, &[])
}
