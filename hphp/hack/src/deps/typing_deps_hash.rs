// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::hash::Hasher;

use fnv::FnvHasher;

mod typing_deps_kind;

pub use typing_deps_kind::DepKind as DepType;

impl DepType {
    pub fn as_u8(self) -> u8 {
        self as u8
    }

    pub fn from_u8(tag: u8) -> Option<Self> {
        Self::from_repr(tag)
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

#[cfg(test)]
mod tests {
    use ocamlrep::FromOcamlRep;
    use ocamlrep::Value;

    use super::DepType;

    #[test]
    fn kconst_ocaml_ordinal_differs_from_hash_tag() {
        let kind = DepType::from_ocamlrep(Value::int(4)).unwrap();
        assert_eq!(kind, DepType::KConst);
        assert_eq!(kind.as_u8(), 5);
    }
}
