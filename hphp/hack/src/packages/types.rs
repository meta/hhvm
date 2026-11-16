// Copyright (c) Meta, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::ops::Deref;
use std::ops::DerefMut;
use std::sync::LazyLock;

use hash::IndexMap;
use hash::IndexSet;
use serde::Deserialize;
use toml::Spanned;
// Preserve the order for ease of testing
// Alternatively, we could use HashMap for performance
pub type PackageMap = IndexMap<Spanned<String>, Package>;
pub type DeploymentMap = IndexMap<Spanned<String>, Deployment>;
pub type ImplicitPackageMap = IndexMap<Spanned<String>, ImplicitPackage>;

#[derive(Debug, Default, Deserialize, Clone, Eq, PartialEq)]
pub struct NameSet(IndexSet<Spanned<String>>);

#[derive(Debug, Deserialize, Clone, PartialEq, Eq)]
pub struct Package {
    pub includes: Option<NameSet>,
    pub soft_includes: Option<NameSet>,
    pub include_paths: Option<NameSet>,
    /// When true, this package opts into strict isolation (see the OCaml
    /// `Package.t` for the enforced semantics). Defaults to false when the
    /// `enable_strict_isolation` key is absent from the `[packages.*]` stanza.
    #[serde(default)]
    pub enable_strict_isolation: bool,
    /// Opt-in (see the OCaml `Package.t`); absent means not checkable.
    #[serde(default)]
    pub allow_deployed_packages_checking: bool,
    pub raise_dynamic_class_load_error: Option<bool>,
}

/// A single `[implicit_packages.<family>]` stanza. It declares a *family* of
/// packages: every direct child directory `D` of `path` denotes a synthesized
/// package `<family>.D` whose `include_paths` is `path/D/` and whose
/// `includes` / `soft_includes` are the ones declared here.
///
/// The members are NEVER materialized at parse time — they are synthesized
/// lazily during package lookup (see `package_info.ml`). This struct therefore
/// only carries the family-level declaration.
#[derive(Debug, Deserialize, Clone, PartialEq, Eq)]
pub struct ImplicitPackage {
    pub path: Spanned<String>,
    pub includes: Option<NameSet>,
    pub soft_includes: Option<NameSet>,
    pub raise_dynamic_class_load_error: Option<bool>,
    /// `include_paths` is NOT a valid field on an implicit_packages entry (the
    /// include paths are derived from `path`). We accept it during
    /// deserialization only so we can emit a precise error if a user specifies
    /// it, rather than silently ignoring it.
    pub include_paths: Option<NameSet>,
}

#[derive(Debug, Deserialize)]
pub struct Deployment {
    pub packages: Option<NameSet>,
    pub soft_packages: Option<NameSet>,
}

/// The top-level `[strict_isolation]` table.
#[derive(Debug, Default, Deserialize, Clone, PartialEq, Eq)]
pub struct StrictIsolation {
    /// Paths, in `include_paths` syntax, whose code may load classes in any
    /// strict package by name. None may overlap a strict package or nest under another.
    pub loader_paths: Option<NameSet>,
}

impl<'a> Default for &'a NameSet {
    fn default() -> &'a NameSet {
        static SET: LazyLock<NameSet> = LazyLock::new(|| NameSet(IndexSet::default()));
        &SET
    }
}
impl Deref for NameSet {
    type Target = IndexSet<Spanned<String>>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
impl DerefMut for NameSet {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl Package {
    pub fn should_raise_dynamic_class_load_error(&self) -> bool {
        self.enable_strict_isolation && self.raise_dynamic_class_load_error.unwrap_or(true)
    }
}

impl ImplicitPackage {
    pub fn should_raise_dynamic_class_load_error(&self) -> bool {
        self.raise_dynamic_class_load_error.unwrap_or(true)
    }
}
impl FromIterator<Spanned<String>> for NameSet {
    fn from_iter<I>(iter: I) -> Self
    where
        I: IntoIterator<Item = Spanned<String>>,
    {
        let mut set = IndexSet::default();
        for name in iter {
            set.insert(name);
        }
        NameSet(set)
    }
}
impl Iterator for NameSet {
    type Item = Spanned<String>;

    fn next(&mut self) -> Option<Self::Item> {
        self.0.iter().next().cloned()
    }
}
impl NameSet {
    pub fn take(&mut self, value: &Spanned<String>) -> Option<Spanned<String>> {
        self.0.swap_take(value)
    }
}
