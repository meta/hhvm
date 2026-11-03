// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::fmt::Debug;

use dep::Dep;
use hh24_types::DependencyHash;
use pos::ClassConstName;
use pos::ConstName;
use pos::FunName;
use pos::MethodName;
use pos::ModuleName;
use pos::PropName;
use pos::TypeName;
use typing_deps_hash::DepType;

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(thiserror::Error, Debug)]
pub enum Error {}

// Implementations of `FoldedDeclProvider` need to be able to record
// dependencies (if needed). We do this by having the functions of this
// trait take a "who's asking?" symbol of this type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum DeclName {
    Fun(FunName),
    Const(ConstName),
    Type(TypeName),
    Module(ModuleName),
}

impl DeclName {
    pub fn to_dep(&self) -> Dep {
        Dep::new(match self {
            DeclName::Fun(n) => typing_deps_hash::hash1(DepType::KFun, n.as_str().as_bytes()),
            DeclName::Const(n) => typing_deps_hash::hash1(DepType::KGConst, n.as_str().as_bytes()),
            DeclName::Type(n) => typing_deps_hash::hash1(DepType::KType, n.as_str().as_bytes()),
            DeclName::Module(n) => typing_deps_hash::hash1(DepType::KModule, n.as_str().as_bytes()),
        })
    }
}

impl From<FunName> for DeclName {
    fn from(name: FunName) -> Self {
        Self::Fun(name)
    }
}

impl From<ConstName> for DeclName {
    fn from(name: ConstName) -> Self {
        Self::Const(name)
    }
}

impl From<ModuleName> for DeclName {
    fn from(name: ModuleName) -> Self {
        Self::Module(name)
    }
}

impl From<TypeName> for DeclName {
    fn from(name: TypeName) -> Self {
        Self::Type(name)
    }
}

// nb(sf, 2022-03-15): c.f. ` Typing_deps.Dep.variant`
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
/// A node in the dependency graph that, when changed, must recheck all of its
/// dependents.
pub enum DependencyName {
    /// Represents another class depending on a class via an inheritance-like
    /// mechanism (`extends`, `implements`, `use`, `require extends`, `require
    /// implements`, etc.)
    Extends(TypeName),
    /// Represents something depending on a class constant.
    Const(TypeName, ClassConstName),
    /// Represents something depending on a class constructor.
    Constructor(TypeName),
    /// Represents something depending on a class's instance property.
    Prop(TypeName, PropName),
    /// Represents something depending on a class's static property.
    StaticProp(TypeName, PropName),
    /// Represents something depending on a class's instance method.
    Method(TypeName, MethodName),
    /// Represents something depending on a class's static method.
    StaticMethod(TypeName, MethodName),
    /// Represents something depending on all members of a class. Particularly
    /// useful for switch exhaustiveness-checking. We establish a dependency on
    /// all members of an enum in that case.
    AllMembers(TypeName),
    /// Represents something depending on a global constant.
    GConst(ConstName),
    /// Represents something depending on a global function.
    Fun(FunName),
    /// Represents something depending on a class/typedef/trait/interface
    Type(TypeName),
    /// Represents something depending on a module.
    Module(ModuleName),
}

impl From<FunName> for DependencyName {
    fn from(name: FunName) -> Self {
        Self::Fun(name)
    }
}

impl From<ConstName> for DependencyName {
    fn from(name: ConstName) -> Self {
        Self::GConst(name)
    }
}

impl From<ModuleName> for DependencyName {
    fn from(name: ModuleName) -> Self {
        Self::Module(name)
    }
}

impl From<TypeName> for DependencyName {
    fn from(name: TypeName) -> Self {
        Self::Type(name)
    }
}

impl From<DeclName> for DependencyName {
    fn from(name: DeclName) -> Self {
        match name {
            DeclName::Fun(name) => DependencyName::Fun(name),
            DeclName::Const(name) => DependencyName::GConst(name),
            DeclName::Type(name) => DependencyName::Type(name),
            DeclName::Module(name) => DependencyName::Module(name),
        }
    }
}

impl From<DependencyName> for DependencyHash {
    // Keep in sync with Dep.make in typing_deps.ml
    fn from(name: DependencyName) -> Self {
        match name {
            DependencyName::Extends(t) => Self::of_symbol(DepType::KExtends, &t),
            DependencyName::Const(t, c) => Self::of_member(DepType::KConst, t.into(), &c),
            DependencyName::Constructor(t) => Self::of_member(DepType::KConstructor, t.into(), ""),
            DependencyName::Prop(t, p) => Self::of_member(DepType::KProp, t.into(), &p),
            DependencyName::StaticProp(t, p) => Self::of_member(DepType::KSProp, t.into(), &p),
            DependencyName::Method(t, m) => Self::of_member(DepType::KMethod, t.into(), &m),
            DependencyName::StaticMethod(t, m) => Self::of_member(DepType::KSMethod, t.into(), &m),
            DependencyName::AllMembers(t) => Self::of_member(DepType::KAllMembers, t.into(), ""),
            DependencyName::GConst(c) => Self::of_symbol(DepType::KGConst, &c),
            DependencyName::Fun(f) => Self::of_symbol(DepType::KFun, &f),
            DependencyName::Type(t) => Self::of_symbol(DepType::KType, &t),
            DependencyName::Module(m) => Self::of_symbol(DepType::KModule, &m),
        }
    }
}

impl DependencyName {
    pub fn to_dep(&self) -> Dep {
        Dep::new(hh24_types::DependencyHash::from(*self).0)
    }

    // Keep in sync with Dep.extract_name in typing_deps.ml
    pub fn extract_name(&self) -> String {
        use core_utils_rust::strip_ns;
        match self {
            DependencyName::Type(t)
            | DependencyName::Extends(t)
            | DependencyName::Constructor(t)
            | DependencyName::AllMembers(t) => strip_ns(t).into(),
            DependencyName::Const(t, c) => format!("{}::{}", strip_ns(t), c),
            DependencyName::Prop(t, p) | DependencyName::StaticProp(t, p) => {
                format!("{}::{}", strip_ns(t), p)
            }
            DependencyName::Method(t, m) | DependencyName::StaticMethod(t, m) => {
                format!("{}::{}", strip_ns(t), m)
            }
            DependencyName::GConst(c) => strip_ns(c).into(),
            DependencyName::Fun(f) => strip_ns(f).into(),
            DependencyName::Module(m) => m.as_str().into(),
        }
    }

    pub fn dep_type(&self) -> DepType {
        match self {
            DependencyName::Extends(_) => DepType::KExtends,
            DependencyName::Const(_, _) => DepType::KConst,
            DependencyName::Constructor(_) => DepType::KConstructor,
            DependencyName::Prop(_, _) => DepType::KProp,
            DependencyName::StaticProp(_, _) => DepType::KSProp,
            DependencyName::Method(_, _) => DepType::KMethod,
            DependencyName::StaticMethod(_, _) => DepType::KSMethod,
            DependencyName::AllMembers(_) => DepType::KAllMembers,
            DependencyName::GConst(_) => DepType::KGConst,
            DependencyName::Fun(_) => DepType::KFun,
            DependencyName::Type(_) => DepType::KType,
            DependencyName::Module(_) => DepType::KModule,
        }
    }
}

/// Organize and administer dependency records.
pub trait DepGraphWriter: Debug + Send + Sync {
    /// Record a dependency.
    // e.g. If class B extends A {} then A <- B (B depends on A). So here,
    // dependent is B and dependency is A.
    fn add_dependency(&self, dependent: DeclName, dependency: DependencyName) -> Result<()>;
}

/// Query dependency records.
pub trait DepGraphReader: Debug + Send + Sync {
    /// Retrieve dependents of a name.
    fn get_dependents(&self, dependency: DependencyName) -> Box<dyn Iterator<Item = Dep> + '_>;
}

/// A no-op implementation of the `DepGraphReader` & `DepGraphWriter` traits.
/// All registered edges are thrown away.
#[derive(Debug, Clone, Default)]
pub struct NoDepGraph;

impl DepGraphWriter for NoDepGraph {
    fn add_dependency(&self, _dependent: DeclName, _dependency: DependencyName) -> Result<()> {
        Ok(())
    }
}

impl DepGraphReader for NoDepGraph {
    fn get_dependents(&self, _dependency: DependencyName) -> Box<dyn Iterator<Item = Dep>> {
        Box::new(std::iter::empty())
    }
}

/// Read & write dependency records.
pub trait DepGraph: DepGraphReader + DepGraphWriter {}
impl<T: DepGraphReader + DepGraphWriter> DepGraph for T {}
