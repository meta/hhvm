// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::fmt::Display;
use std::fmt::Formatter;
use std::fmt::LowerHex;

use ocamlrep::Allocator;
use ocamlrep::FromError;
use ocamlrep::FromOcamlRep;
use ocamlrep::ToOcamlRep;
use ocamlrep::Value;
use ocamlrep::from;
use serde::Deserialize;
use serde::Serialize;
use typing_deps_hash::DepType;
use typing_deps_hash::declares_hash;
use typing_deps_hash::hash2;

#[repr(transparent)]
#[derive(Clone, Copy, Default, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
#[derive(bytemuck::Pod, bytemuck::Zeroable)]
#[derive(Serialize, Deserialize)]
pub struct Dep(u64);

impl Dep {
    pub fn new(x: u64) -> Self {
        Dep(x)
    }

    pub fn is_class(self) -> bool {
        (self.0 & 1) != 0
    }

    // Return the 'Extends' dep for a class from its 'Type' dep
    pub fn class_to_extends(self) -> Option<Self> {
        if !self.is_class() {
            None
        } else {
            Some(Dep(self.0 & (!1)))
        }
    }

    // Return the 'NotSubtype' dep for a class from its 'Type' dep
    pub fn class_to_not_subtype(self) -> Self {
        Dep(typing_deps_hash::hash2(DepType::KNotSubtype, self.0, &[]))
    }

    /// The `Declares` special dep, used to mark declared members.
    pub fn declares() -> Self {
        Dep(declares_hash())
    }

    /// Whether a dep is the `Declares` special dep.
    pub fn is_declares(&self) -> bool {
        *self == Self::declares()
    }

    /// Assuming `self` is the dep of a type `A`, returns the dep of
    /// member `name` in `A` with member kind `dep_type`.
    pub fn member(&self, dep_type: DepType, name: &str) -> Self {
        Dep(hash2(dep_type, self.0, name.as_bytes()))
    }

    #[inline]
    pub fn from_u64_slice(s: &[u64]) -> &[Dep] {
        bytemuck::cast_slice(s)
    }
}

impl Display for Dep {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        Display::fmt(&self.0, f)
    }
}

impl LowerHex for Dep {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        LowerHex::fmt(&self.0, f)
    }
}

impl From<Dep> for u64 {
    fn from(dep: Dep) -> Self {
        dep.0
    }
}

impl FromOcamlRep for Dep {
    fn from_ocamlrep(value: Value<'_>) -> Result<Self, FromError> {
        let x: isize = from::expect_int(value)?;
        // In Rust, a numeric cast between two integers of the same size
        // is a no-op. We require a 64-bit word size.
        let x = x as u64;
        // The conversion from an OCaml integer to a Rust integer involves a
        // right arithmetic bitshift. Due to sign extension, if the OCaml
        // integer was negative, the resulting Rust integer's MSB will be set.
        // We don't want that, because we are disguising unsigned 63-bit Rust
        // integers as signed 63-bit integers in OCaml.
        let x = x & !(1 << 63);
        Ok(Dep(x))
    }
}

impl ToOcamlRep for Dep {
    fn to_ocamlrep<'a, A: Allocator>(&'a self, _alloc: &'a A) -> Value<'a> {
        let x: u64 = self.0;
        // In Rust, a numeric cast between two integers of the same size
        // is a no-op. We require a 64-bit word size.
        let x = x as isize;
        Value::int(x)
    }
}
