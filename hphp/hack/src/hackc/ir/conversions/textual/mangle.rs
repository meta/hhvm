// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::cmp::Ordering;
use std::fmt;

use naming_special_names_rust::members;

use crate::class::IsStatic;

pub(crate) const TOP_LEVELS_CLASS: &str = "$root";

/// Used for things that can mangle themselves directly.
pub(crate) trait Mangle {
    fn mangle(&self) -> String;
}

fn starts_like_a_textual_ident(name: &[u8]) -> bool {
    name.first().is_some_and(|&x| x == b'n') && name.len() > 1 && name[1].is_ascii_digit()
}

//precondition: name should not contain non-utf8 chars
fn add_mangling_prefix(name: &[u8]) -> String {
    // We mangle by adding the prefix 'mangled:::'.
    // No collision expected because ':::' is not a valid identifier Hack substring
    // We can not use '::' because we already introduce '::' when removing Hack namespace
    // separator '//' and the current mangling could by applied on a function name.
    let mut res = String::from("mangled:::");
    res.push_str(std::str::from_utf8(name).unwrap());
    res
}

impl Mangle for [u8] {
    fn mangle(&self) -> String {
        // Handle some reserved tokens.
        match self {
            b"_" | b"declare" | b"define" | b"else" | b"extends" | b"false" | b"float"
            | b"global" | b"handlers" | b"if" | b"int" | b"jmp" | b"load" | b"local" | b"null"
            | b"prune" | b"ret" | b"store" | b"then" | b"throw" | b"true" | b"type"
            | b"unreachable" | b"void" => add_mangling_prefix(self),
            _ if starts_like_a_textual_ident(self) => add_mangling_prefix(self),
            _ => {
                // This mangling is terrible... but probably "good enough".
                // If a digit is first then we prepend a '_'.
                // [A-Za-z0-9_$] -> identity
                // \ -> ::
                // anything else -> xx (hex digits)
                let mut res = String::with_capacity(self.len());
                if self.first().is_some_and(u8::is_ascii_digit) {
                    res.push('_');
                }
                for &ch in self {
                    match ch {
                        b'_' | b'$' | b':' => res.push(ch as char),
                        b'\\' => {
                            res.push(':');
                            res.push(':');
                        }
                        ch if ch.is_ascii_alphanumeric() => res.push(ch as char),
                        _ => {
                            res.push(b"0123456789abcdef"[(ch >> 4) as usize] as char);
                            res.push(b"0123456789abcdef"[(ch & 15) as usize] as char);
                        }
                    }
                }
                res
            }
        }
    }
}

impl Mangle for str {
    fn mangle(&self) -> String {
        self.as_bytes().mangle()
    }
}

// Classes and functions live in different namespaces.

impl Mangle for ir::PropName {
    fn mangle(&self) -> String {
        self.as_bytes().mangle()
    }
}

#[derive(Eq, PartialEq, Hash, Clone, Debug)]
pub(crate) enum Intrinsic {
    ConstInit(ir::ClassName),
    Construct(ir::ClassName),
    Factory(ir::ClassName),
    Invoke(TypeName),
    PropInit(ir::ClassName),
    StaticConstInit(ir::ClassName),
    StaticInit(ir::ClassName),
}

impl Intrinsic {
    pub(crate) fn contains_unknown(&self) -> bool {
        match self {
            Intrinsic::ConstInit(_)
            | Intrinsic::Construct(_)
            | Intrinsic::Factory(_)
            | Intrinsic::PropInit(_)
            | Intrinsic::StaticConstInit(_)
            | Intrinsic::StaticInit(_) => false,
            Intrinsic::Invoke(name) => *name == TypeName::Unknown,
        }
    }
}

#[derive(Debug, Clone, Hash, Eq, PartialEq)]
pub(crate) enum FieldName {
    Prop(ir::PropName),
    Raw(String),
}

impl FieldName {
    pub(crate) fn prop(pid: ir::PropName) -> Self {
        Self::Prop(pid)
    }

    pub(crate) fn raw<'a>(name: impl Into<std::borrow::Cow<'a, str>>) -> Self {
        let name = name.into();
        FieldName::Raw(name.into_owned())
    }

    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtFieldName(self)
    }
}

struct FmtFieldName<'a>(&'a FieldName);

impl fmt::Display for FmtFieldName<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtFieldName(name) = *self;
        match name {
            FieldName::Prop(p) => f.write_str(&p.as_bytes().mangle()),
            FieldName::Raw(s) => f.write_str(s),
        }
    }
}

/// Represents a named callable thing.  This includes top-level functions and
/// methods.
#[derive(Eq, PartialEq, Hash, Clone, Debug)]
pub(crate) enum FunctionName {
    Builtin(crate::hack::Builtin),
    Function(ir::FunctionName),
    Intrinsic(Intrinsic),
    Method(TypeName, ir::MethodName),
    Unmangled(String),
}

impl FunctionName {
    pub(crate) fn method(
        class: ir::ClassName,
        is_static: IsStatic,
        method: ir::MethodName,
    ) -> Self {
        Self::Method(TypeName::class(class, is_static), method)
    }

    pub(crate) fn untyped_method(method: ir::MethodName) -> Self {
        Self::Method(TypeName::Unknown, method)
    }

    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtFunctionName(self)
    }

    pub(crate) fn cmp(&self, other: &Self) -> Ordering {
        let a = self.display().to_string();
        let b = other.display().to_string();
        a.cmp(&b)
    }

    pub(crate) fn contains_unknown(&self) -> bool {
        match self {
            FunctionName::Builtin(_) | FunctionName::Function(_) | FunctionName::Unmangled(_) => {
                false
            }
            FunctionName::Intrinsic(intrinsic) => intrinsic.contains_unknown(),
            FunctionName::Method(name, _) => *name == TypeName::Unknown,
        }
    }
}

struct FmtFunctionName<'a>(&'a FunctionName);

impl fmt::Display for FmtFunctionName<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtFunctionName(name) = *self;
        match name {
            FunctionName::Builtin(b) => b.as_str().fmt(f)?,
            FunctionName::Function(fid) => {
                write!(f, "{TOP_LEVELS_CLASS}.{}", fid.as_str().as_bytes().mangle())?
            }
            FunctionName::Intrinsic(intrinsic) => {
                let tn;
                let (ty, name) = match intrinsic {
                    Intrinsic::ConstInit(cid) => {
                        tn = TypeName::StaticClass(*cid);
                        (Some(&tn), "_86cinit")
                    }
                    Intrinsic::Construct(cid) => {
                        tn = TypeName::Class(*cid);
                        (Some(&tn), members::__CONSTRUCT)
                    }
                    Intrinsic::Factory(cid) => {
                        tn = TypeName::StaticClass(*cid);
                        (Some(&tn), "__factory")
                    }
                    Intrinsic::Invoke(tn) => (Some(tn), "__invoke"),
                    Intrinsic::PropInit(cid) => {
                        tn = TypeName::Class(*cid);
                        (Some(&tn), "_86pinit")
                    }
                    Intrinsic::StaticConstInit(cid) => {
                        tn = TypeName::StaticClass(*cid);
                        (Some(&tn), "_86constinit")
                    }
                    Intrinsic::StaticInit(cid) => {
                        tn = TypeName::StaticClass(*cid);
                        (Some(&tn), "_86sinit")
                    }
                };
                if let Some(ty) = ty {
                    write!(f, "{}.{}", ty.display(), name)?;
                } else {
                    f.write_str(name)?;
                }
            }
            FunctionName::Method(class, mid) => {
                write!(f, "{}.{}", class.display(), mid.as_bytes().mangle())?;
            }
            FunctionName::Unmangled(s) => s.fmt(f)?,
        }
        Ok(())
    }
}

#[derive(Eq, PartialEq, Hash, Clone, Debug)]
pub(crate) enum GlobalName {
    Global(ir::GlobalId),
    GlobalConst(ir::GlobalId),
}

impl GlobalName {
    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtGlobalName(self)
    }

    pub(crate) fn cmp(&self, other: &Self) -> Ordering {
        let a = self.display().to_string();
        let b = other.display().to_string();
        a.cmp(&b)
    }
}

struct FmtGlobalName<'a>(&'a GlobalName);

impl fmt::Display for FmtGlobalName<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtGlobalName(name) = *self;
        match name {
            GlobalName::Global(id) => {
                write!(f, "global::{}", id.as_bytes().mangle())
            }
            GlobalName::GlobalConst(id) => {
                write!(f, "gconst::{}", id.as_bytes().mangle())
            }
        }
    }
}

#[derive(Debug, Clone, Hash, Eq, PartialEq)]
pub(crate) enum TypeName {
    Class(ir::ClassName),
    Curry(Box<FunctionName>),
    StaticClass(ir::ClassName),
    Unknown,
    UnmangledRef(&'static str),
}

impl TypeName {
    pub(crate) fn class(class: ir::ClassName, is_static: crate::class::IsStatic) -> Self {
        match is_static {
            IsStatic::Static => Self::StaticClass(class),
            IsStatic::NonStatic => Self::Class(class),
        }
    }

    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtTypeName(self)
    }
}

struct FmtTypeName<'a>(&'a TypeName);

impl fmt::Display for FmtTypeName<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtTypeName(name) = *self;
        match name {
            TypeName::Class(cid) => f.write_str(&cid.as_str().as_bytes().mangle()),
            TypeName::Curry(deref!(FunctionName::Function(fid))) => {
                write!(f, "{}$curry", fid.as_str().as_bytes().mangle())
            }
            TypeName::Curry(deref!(FunctionName::Method(ty, mid))) => {
                write!(f, "{}_{}$curry", ty.display(), mid.as_bytes().mangle())
            }
            TypeName::Curry(_) => panic!("Unable to name curry type {name:?}"),
            TypeName::Unknown => f.write_str("?"),
            TypeName::StaticClass(cid) => {
                f.write_str(&cid.as_str().as_bytes().mangle())?;
                f.write_str("$static")
            }
            TypeName::UnmangledRef(s) => s.fmt(f),
        }
    }
}

#[derive(Clone, Debug, Hash, Eq, PartialEq)]
pub(crate) enum VarName {
    Global(GlobalName),
    Local(ir::LocalId),
}

impl VarName {
    pub(crate) fn global(s: GlobalName) -> Self {
        Self::Global(s)
    }

    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtVarName(self)
    }
}

impl From<ir::LocalId> for VarName {
    fn from(lid: ir::LocalId) -> Self {
        Self::Local(lid)
    }
}

struct FmtVarName<'a>(&'a VarName);

impl fmt::Display for FmtVarName<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtVarName(var) = *self;
        match *var {
            VarName::Global(ref s) => s.display().fmt(f),
            VarName::Local(lid) => FmtLid(lid).fmt(f),
        }
    }
}

struct FmtLid(pub ir::LocalId);

impl fmt::Display for FmtLid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtLid(lid) = *self;
        match lid {
            ir::LocalId::Named(id) => {
                let name = id.as_str().mangle();
                f.write_str(&name)
            }
            ir::LocalId::Unnamed(id) => {
                write!(f, "${}", id.as_usize())
            }
        }
    }
}
