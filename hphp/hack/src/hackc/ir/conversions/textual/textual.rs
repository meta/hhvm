// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! This is a structural version of the Textual language - it should have
//! basically no business logic and just provides a type-safe way to write
//! Textual.

use std::borrow::Cow;
use std::fmt;
use std::path::Path;

use anyhow::Error;
use ascii::AsciiString;
use hash::HashMap;
use hash::HashSet;
use ir::BlockId;
use ir::EnforceableType;
use ir::FloatBits;
use ir::LocalId;
use ir::SrcLoc;
use ir::Typedef;
use ir::VerifyRetKind;
use itertools::Itertools;
use newtype::newtype_int;
use strum::EnumProperty;

use crate::hack;
use crate::mangle::FieldName;
use crate::mangle::FunctionName;
use crate::mangle::GlobalName;
use crate::mangle::Intrinsic;
use crate::mangle::Mangle;
use crate::mangle::TypeName;
use crate::mangle::VarName;
use crate::types::convert_ty;

pub(crate) const INDENT: &str = "  ";
pub(crate) const NOTNULL: &str = ".notnull";
pub(crate) const VARIADIC: &str = ".variadic";
pub(crate) const TYPEVAR: &str = ".typevar";

type Result<T = (), E = Error> = std::result::Result<T, E>;

pub(crate) struct TextualFile<'a> {
    w: &'a mut dyn std::io::Write,
    pub(crate) hide_static_coeffects: bool,
    pub(crate) enable_var_cache: bool,
    filename: &'a Path,
    pub(crate) internal_functions: HashSet<FunctionName>,
    pub(crate) called_functions: HashSet<FunctionName>,
    pub(crate) internal_globals: HashMap<GlobalName, Ty>,
    pub(crate) referenced_globals: HashSet<GlobalName>,
    pub(crate) curry_tys: HashSet<CurryTy>,
}

impl<'a> TextualFile<'a> {
    pub(crate) fn new(
        w: &'a mut dyn std::io::Write,
        hide_static_coeffects: bool,
        enable_var_cache: bool,
        filename: &'a Path,
    ) -> Self {
        TextualFile {
            w,
            hide_static_coeffects,
            enable_var_cache,
            filename,
            internal_functions: Default::default(),
            called_functions: Default::default(),
            internal_globals: Default::default(),
            referenced_globals: Default::default(),
            curry_tys: Default::default(),
        }
    }
}

impl TextualFile<'_> {
    pub(crate) fn debug_separator(&mut self) -> Result {
        writeln!(self.w)?;
        Ok(())
    }

    pub(crate) fn declare_alias(&mut self, typedef: Typedef) -> Result {
        write!(self.w, "type {} equals ", typedef.name.as_str().mangle())?;
        // This is pretty hacky - just grabbing something printable from wherever I can find it
        // In the general case we have a list of types (for case types) but
        // Infer won't initially know what to do with more than one type
        let mut sep = "";
        for ti in typedef.type_info_union {
            let et = EnforceableType::from_type_info(&ti);
            match convert_ty(&et).try_deref() {
                None => (),
                Some(ty) => {
                    write!(self.w, "{sep}{}", ty.display())?;
                    sep = ", ";
                }
            }
        }
        write!(self.w, "\n")?;
        Ok(())
    }

    pub(crate) fn declare_function(
        &mut self,
        name: &FunctionName,
        attributes: &FuncAttributes,
        tys: &[Ty],
        ret_ty: &Ty,
    ) -> Result {
        write!(self.w, "declare ")?;

        if attributes.is_final {
            write!(self.w, ".final ")?;
        }

        if attributes.is_async {
            write!(self.w, ".async ")?;
        }

        if attributes.is_wrapper {
            write!(self.w, ".wrapper ")?;
        }

        if attributes.is_abstract {
            write!(self.w, ".abstract ")?;
        }

        write!(self.w, "{}(", name.display())?;

        // TODO: For now textual can't handle a mix of types with a trailing
        // ellipsis.
        if tys.contains(&Ty::Ellipsis) {
            write!(self.w, "...")?;
        } else {
            let mut sep = "";
            for ty in tys {
                write!(self.w, "{sep}{}", ty.display())?;
                sep = ", ";
            }
        }

        writeln!(self.w, "): {}", ret_ty.display())?;
        Ok(())
    }

    fn declare_unknown_function(&mut self, name: &FunctionName) -> Result {
        writeln!(
            self.w,
            "declare {name}(...): *HackMixed",
            name = name.display()
        )?;
        Ok(())
    }

    fn declare_unknown_global(&mut self, name: &GlobalName) -> Result {
        writeln!(
            self.w,
            "global {name} : {ty}",
            name = name.display(),
            ty = Ty::SpecialPtr(SpecialTy::Mixed).display()
        )?;
        Ok(())
    }

    pub(crate) fn define_function<R>(
        &mut self,
        name: &FunctionName,
        loc: Option<&SrcLoc>,
        attributes: &FuncAttributes,
        coeffects: &[ir::Ctx],
        params: &[Param<'_>],
        ret_ty: &Return<'_>,
        locals: &[(LocalId, &Ty)],
        body: impl FnOnce(&mut FuncBuilder<'_, '_>) -> Result<R>,
    ) -> Result<R> {
        if !self.internal_functions.contains(name) {
            self.internal_functions.insert(name.clone());
        }

        if let Some(loc) = loc {
            self.write_full_loc(loc)?;
        }

        write!(self.w, "define ")?;

        if attributes.is_final {
            write!(self.w, ".final ")?;
        }

        if attributes.is_async {
            write!(self.w, ".async ")?;
        }

        if attributes.is_wrapper {
            write!(self.w, ".wrapper ")?;
        }

        if attributes.is_abstract {
            write!(self.w, ".abstract ")?;
        }

        if attributes.is_curry {
            write!(self.w, ".curry ")?;
        }

        if !self.hide_static_coeffects && !coeffects.is_empty() {
            write!(self.w, ".static_coeffects = ")?;
            let mut sep = "";
            for ctx in coeffects.iter() {
                write!(self.w, "{}\"{}\"", sep, ctx)?;
                sep = ", ";
            }
            write!(self.w, " ")?;
        }

        write!(self.w, "{}(", name.display())?;

        let mut sep = "";
        for param in params {
            write!(self.w, "{sep}{name}: ", name = param.name.display(),)?;
            if let Some(attrs) = param.attrs.as_ref() {
                for attr in attrs.iter() {
                    write!(self.w, "{attr} ")?;
                }
            }
            write!(self.w, "{ty}", ty = param.ty.display())?;
            sep = ", ";
        }
        write!(self.w, ") : ")?;
        if let Some(attrs) = ret_ty.attrs.as_ref() {
            for attr in attrs.iter() {
                write!(self.w, "{attr} ")?;
            }
        }
        writeln!(self.w, "{} {{", ret_ty.ty.display())?;

        if !locals.is_empty() {
            let mut sep = "";
            write!(self.w, "local ")?;
            for (lid, ty) in locals {
                write!(
                    self.w,
                    "{sep}{name}: {ty}",
                    name = VarName::Local(*lid).display(),
                    ty = ty.display()
                )?;
                sep = ", ";
            }
            writeln!(self.w)?;
        }

        let cache: Box<dyn VarCache> = if self.enable_var_cache {
            Box::<HashMapVarCache>::default()
        } else {
            Box::<NoVarCache>::default()
        };

        let mut writer = FuncBuilder {
            cur_loc: loc.cloned(),
            next_id: Sid::from_usize(0),
            txf: self,
            cache,
        };

        writer.write_label(BlockId::from_usize(0), &[])?;
        let result = body(&mut writer)?;

        writeln!(self.w, "}}")?;
        writeln!(self.w)?;
        Ok(result)
    }

    #[allow(dead_code)]
    pub(crate) fn define_global(&mut self, name: GlobalName, ty: Ty) {
        self.internal_globals.insert(name, ty);
    }

    pub(crate) fn define_type<'s>(
        &mut self,
        name: &TypeName,
        src_loc: Option<&SrcLoc>,
        extends: impl Iterator<Item = &'s TypeName>,
        fields: impl Iterator<Item = Field<'s>>,
        metadata: impl Iterator<Item = (&'s str, &'s Const)>,
    ) -> Result {
        if let Some(src_loc) = src_loc {
            self.write_full_loc(src_loc)?;
        }

        write!(self.w, "type {}", name.display())?;

        let mut sep = " extends";
        for base in extends {
            write!(self.w, "{sep} {}", base.display())?;
            sep = ",";
        }

        write!(self.w, " = ")?;
        self.write_metadata(metadata)?;
        write!(self.w, "{{")?;

        let mut sep = "\n";

        for f in fields {
            for comment in &f.comments {
                writeln!(self.w, "{sep}{INDENT}// {comment}")?;
                sep = "";
            }
            write!(
                self.w,
                "{sep}{INDENT}{name}: {vis} ",
                name = f.name.display(),
                vis = f.visibility.decl()
            )?;

            for attr in &f.attributes {
                write!(self.w, "{} ", attr.display())?;
            }

            write!(self.w, "{ty}", ty = f.ty.display())?;
            sep = ";\n";
        }

        writeln!(self.w, "\n}}")?;
        writeln!(self.w)?;

        Ok(())
    }

    pub(crate) fn set_attribute(&mut self, attr: FileAttribute) -> Result {
        match attr {
            FileAttribute::SourceLanguage(lang) => {
                writeln!(self.w, ".source_language = \"{lang}\"")?;
            }
        }
        Ok(())
    }

    pub(crate) fn write_comment(&mut self, msg: &str) -> Result {
        writeln!(self.w, "// {msg}")?;
        Ok(())
    }

    fn write_curry_definition(&mut self, curry: CurryTy) -> Result {
        let curry_ty = curry.ty_name();
        let mut fields = Vec::new();
        if curry.virtual_call {
            let FunctionName::Method(captured_this_ty, _) = &curry.name else {
                unreachable!();
            };
            fields.push(Field {
                name: FieldName::raw("this"),
                ty: Ty::named_type_ptr(captured_this_ty.clone()).into(),
                visibility: Visibility::Private,
                attributes: Default::default(),
                comments: Default::default(),
            });
        }

        for (idx, ty) in curry.arg_tys.iter().enumerate() {
            fields.push(Field {
                name: FieldName::raw(format!("arg{idx}")),
                ty: ty.into(),
                visibility: Visibility::Private,
                attributes: Default::default(),
                comments: Default::default(),
            });
        }

        let metadata_class: Const = "class".into();
        let metadata_false: Const = false.into();
        let metadata_true: Const = true.into();
        let metadata = vec![
            ("kind", &metadata_class),
            ("static", &metadata_false),
            ("final", &metadata_true),
        ];

        self.define_type(
            &curry_ty,
            None,
            std::iter::empty(),
            fields.into_iter(),
            metadata.into_iter(),
        )?;

        const THIS_NAME: &str = "this";
        let this_lid = LocalId::Named(ir::intern(THIS_NAME));
        let this_ty = Ty::Type(curry_ty.clone());
        let this_ty_ptr = Ty::named_type_ptr(curry_ty.clone());

        const VARARGS_NAME: &str = "args";
        let varargs_lid = LocalId::Named(ir::intern(VARARGS_NAME));
        let args_ty = Ty::SpecialPtr(SpecialTy::Vec);

        let params = vec![
            // ignored 'this' parameter
            Param {
                name: VarName::Local(this_lid),
                attrs: None,
                ty: this_ty_ptr.clone(),
            },
            Param {
                name: VarName::Local(varargs_lid),
                attrs: Some(vec![Cow::Borrowed(VARIADIC)].into_boxed_slice()),
                ty: args_ty.clone(),
            },
        ];
        let ret_ty = Return::new(Ty::mixed_ptr());
        let method = FunctionName::Intrinsic(Intrinsic::Invoke(curry_ty));
        let attrs = FuncAttributes {
            is_async: false,
            is_curry: true,
            is_final: true,
            is_abstract: false,
            is_wrapper: false,
        };
        self.define_function(&method, None, &attrs, &[], &params, &ret_ty, &[], |fb| {
            let this_id = ir::intern(THIS_NAME);
            let this = fb.load(&this_ty_ptr, Expr::deref(LocalId::Named(this_id)))?;

            let mut args = Vec::new();

            for (idx, ty) in curry.arg_tys.iter().enumerate() {
                let field = FieldName::raw(format!("arg{idx}"));
                let arg = fb.load(ty, Expr::field(this, this_ty.clone(), field))?;
                args.push(arg);
            }

            let varargs_id = ir::intern(VARARGS_NAME);
            let varargs = fb.load(&args_ty, Expr::deref(LocalId::Named(varargs_id)))?;
            args.push(hack::call_builtin(fb, hack::Builtin::SilSplat, [varargs])?);

            let result = if curry.virtual_call {
                let captured_this_ty = match &curry.name {
                    FunctionName::Method(captured_this_ty, _) => captured_this_ty,
                    _ => {
                        unreachable!();
                    }
                };
                let captured_this = fb.load(
                    &Ty::named_type_ptr(captured_this_ty.clone()),
                    Expr::field(this, this_ty.clone(), FieldName::raw("this")),
                )?;
                fb.call_virtual(&curry.name, captured_this.into(), args)?
            } else {
                fb.call_static(&curry.name, Expr::null(), args)?
            };

            fb.ret(result, VerifyRetKind::None)?;
            Ok(())
        })?;

        Ok(())
    }

    pub(crate) fn write_epilogue<T: Eq + std::hash::Hash + Copy>(
        &mut self,
        builtins: &HashMap<FunctionName, T>,
    ) -> Result<HashSet<T>> {
        if !self.internal_globals.is_empty() {
            self.write_comment("----- GLOBALS -----")?;

            for (name, ty) in self
                .internal_globals
                .iter()
                .sorted_by(|(n1, _), (n2, _)| n1.cmp(n2))
            {
                writeln!(
                    self.w,
                    "global {name} : {ty}",
                    name = name.display(),
                    ty = ty.display()
                )?;
            }
            self.debug_separator()?;
        }

        if !self.curry_tys.is_empty() {
            self.write_comment("----- CURRIES -----")?;

            let curry_tys = std::mem::take(&mut self.curry_tys);
            for curry in curry_tys.into_iter().sorted_by(|a, b| a.name.cmp(&b.name)) {
                self.write_curry_definition(curry)?;
            }

            self.debug_separator()?;
        }

        let (builtins, mut non_builtin_fns): (HashSet<T>, Vec<FunctionName>) =
            (&self.called_functions - &self.internal_functions)
                .into_iter()
                .partition_map(|f| match builtins.get(&f as &FunctionName) {
                    Some(b) => itertools::Either::Left(b),
                    None => itertools::Either::Right(f),
                });
        non_builtin_fns.sort_by(|a, b| a.cmp(b));

        let referenced_globals = (&self.referenced_globals
            - &self.internal_globals.keys().cloned().collect())
            .into_iter()
            .sorted_by(|a, b| a.cmp(b))
            .collect_vec();

        if !non_builtin_fns.is_empty() || !referenced_globals.is_empty() {
            self.write_comment("----- EXTERNALS -----")?;
            for name in non_builtin_fns {
                self.declare_unknown_function(&name)?;
            }
            for name in referenced_globals {
                self.declare_unknown_global(&name)?;
            }
            self.debug_separator()?;
        }

        Ok(builtins)
    }

    fn write_full_loc(&mut self, src_loc: &SrcLoc) -> Result {
        writeln!(self.w, "// .file \"{}\"", self.filename.display())?;
        self.write_line_loc(src_loc)?;
        Ok(())
    }

    fn write_line_loc(&mut self, src_loc: &SrcLoc) -> Result {
        writeln!(self.w, "// .line {}", src_loc.line_begin)?;
        if src_loc.col_begin != 0 {
            writeln!(self.w, "// .column {}", src_loc.col_begin)?;
        }
        Ok(())
    }

    fn write_metadata<'s>(
        &mut self,
        metadata: impl Iterator<Item = (&'s str, &'s Const)>,
    ) -> Result {
        for (k, v) in metadata {
            // Special case - for a false value just emit nothing.
            if matches!(v, Const::False) {
                continue;
            }

            self.w.write_all(b".")?;
            self.w.write_all(k.as_bytes())?;
            // Special case - for a true value just emit the key.
            if !matches!(v, Const::True) {
                self.w.write_all(b"=")?;
                self.write_const(v)?;
            }
            self.w.write_all(b" ")?;
        }
        Ok(())
    }
}

impl ExprWriter for TextualFile<'_> {
    fn register_called_function(&mut self, target: &FunctionName) {
        if !target.contains_unknown() && !self.called_functions.contains(target) {
            self.called_functions.insert(target.clone());
        }
    }

    fn register_used_param(&mut self, _param: &Expr) {}
    fn register_used_global(&mut self, _global: &GlobalName) {}

    fn internal_get_writer(&mut self) -> &mut dyn std::io::Write {
        self.w
    }
}

#[derive(Debug, Default)]
pub(crate) struct FuncAttributes {
    pub is_async: bool,
    pub is_curry: bool,
    pub is_final: bool,
    pub is_abstract: bool,
    pub is_wrapper: bool, // is a wrapper for a function with default arguments or for static
}

#[derive(Debug, Hash, PartialEq, Eq)]
pub(crate) struct CurryTy {
    name: FunctionName,
    virtual_call: bool,
    arg_tys: Box<[Ty]>,
}

impl CurryTy {
    fn ty_name(&self) -> TypeName {
        TypeName::Curry(Box::new(self.name.clone()))
    }

    fn ty(&self) -> Ty {
        Ty::Type(self.ty_name())
    }
}

newtype_int!(Sid, u32, SidMap, SidSet);

struct FmtBid(BlockId);

impl fmt::Display for FmtBid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "b{}", self.0.as_usize())
    }
}

struct FmtSid(Sid);

impl fmt::Display for FmtSid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtSid(sid) = *self;
        write!(f, "n{}", sid.as_usize())
    }
}

/// These are special types that could be expressed as Ptr(Box(String)) but are
/// really common.
#[derive(Copy, Debug, Clone, Hash, Eq, PartialEq, EnumProperty)]
pub(crate) enum SpecialTy {
    #[strum(props(UserType = "HackArraykey"))]
    Arraykey,
    #[strum(props(UserType = "HackBool"))]
    Bool,
    #[strum(props(UserType = "HackDict"))]
    Dict,
    #[strum(props(UserType = "HackFloat"))]
    Float,
    #[strum(props(UserType = "HackInt"))]
    Int,
    #[strum(props(UserType = "HackKeyset"))]
    Keyset,
    #[strum(props(UserType = "HackMixed"))]
    Mixed,
    #[strum(props(UserType = "HackNum"))]
    Num,
    #[strum(props(UserType = "HackString"))]
    String,
    #[strum(props(UserType = "HackVec"))]
    Vec,
}

impl SpecialTy {
    fn user_type(&self) -> TypeName {
        TypeName::UnmangledRef(self.get_str("UserType").unwrap())
    }

    fn nullable(&self) -> ThreeValuedBool {
        match self {
            SpecialTy::Mixed => ThreeValuedBool::Yes,
            SpecialTy::Arraykey
            | SpecialTy::Bool
            | SpecialTy::Dict
            | SpecialTy::Float
            | SpecialTy::Int
            | SpecialTy::Keyset
            | SpecialTy::Num
            | SpecialTy::String
            | SpecialTy::Vec => ThreeValuedBool::No,
        }
    }
}

#[derive(Debug, Clone, Hash, Eq, PartialEq)]
pub(crate) enum Ty {
    Ellipsis,
    Float,
    Int,
    Noreturn,
    Ptr(Box<Ty>),
    SpecialPtr(SpecialTy),
    Special(SpecialTy),
    String,
    Type(TypeName),
    Unknown,
    Void,
    VoidPtr,
}

pub(crate) enum ThreeValuedBool {
    Yes,
    No,
    DontKnow,
}

impl Ty {
    pub(crate) fn display<'r>(&'r self) -> impl fmt::Display + 'r {
        FmtTy(self)
    }

    pub(crate) fn deref(&self) -> Ty {
        let Some(ty) = self.try_deref() else {
            panic!("Unable to deref {self:?}");
        };
        ty
    }

    pub(crate) fn try_deref(&self) -> Option<Ty> {
        match self {
            Ty::Ptr(deref!(sub)) => Some(sub.clone()),
            Ty::VoidPtr => Some(Ty::Void),
            Ty::SpecialPtr(special) => Some(Ty::Special(*special)),
            Ty::Float
            | Ty::Int
            | Ty::Noreturn
            | Ty::String
            | Ty::Special(_)
            | Ty::Type(_)
            | Ty::Unknown
            | Ty::Void
            | Ty::Ellipsis => None,
        }
    }

    pub(crate) fn mixed_ptr() -> Ty {
        Ty::SpecialPtr(SpecialTy::Mixed)
    }

    pub(crate) fn named_type_ptr(name: TypeName) -> Ty {
        Self::ptr(Ty::Type(name))
    }

    pub(crate) fn ptr(sub: Ty) -> Ty {
        Ty::Ptr(Box::new(sub))
    }

    pub(crate) fn unknown() -> Ty {
        Ty::Unknown
    }

    pub(crate) fn nullable(&self) -> ThreeValuedBool {
        match self {
            Ty::Ptr(_) | Ty::Type(_) => ThreeValuedBool::DontKnow,
            Ty::VoidPtr | Ty::Unknown | Ty::Void | Ty::Ellipsis => ThreeValuedBool::Yes,
            Ty::Special(special) | Ty::SpecialPtr(special) => special.nullable(),
            Ty::Float | Ty::Int | Ty::Noreturn | Ty::String => ThreeValuedBool::No,
        }
    }
}

impl From<Ty> for std::borrow::Cow<'_, Ty> {
    fn from(ty: Ty) -> Self {
        Self::Owned(ty)
    }
}

impl<'a> From<&'a Ty> for std::borrow::Cow<'a, Ty> {
    fn from(ty: &'a Ty) -> Self {
        Self::Borrowed(ty)
    }
}

struct FmtTy<'a>(&'a Ty);

impl fmt::Display for FmtTy<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let FmtTy(ty) = *self;
        match ty {
            Ty::Ellipsis => write!(f, "..."),
            Ty::Float => write!(f, "float"),
            Ty::Int => write!(f, "int"),
            Ty::Noreturn => f.write_str("noreturn"),
            Ty::Ptr(sub) => write!(f, "*{}", sub.display()),
            Ty::SpecialPtr(special) => write!(f, "*{}", special.user_type().display()),
            Ty::Special(special) => special.user_type().display().fmt(f),
            Ty::String => write!(f, "*string"),
            Ty::Type(s) => s.display().fmt(f),
            Ty::Unknown => f.write_str("?"),
            Ty::Void => f.write_str("void"),
            Ty::VoidPtr => f.write_str("*void"),
        }
    }
}

#[derive(Clone, Debug, Hash, Eq, PartialEq)]
pub(crate) enum Const {
    EnumClassLabel,
    False,
    Float(FloatBits),
    Int(i64),
    /// A LazyClass represents a class that we know the name of but don't
    /// necessarily know is a real class. It's created via C::class.
    LazyClass(TypeName),
    Null,
    String(AsciiString),
    True,
}

impl From<&'static str> for Const {
    fn from(s: &'static str) -> Self {
        Const::String(AsciiString::from_ascii(s).unwrap())
    }
}

impl From<AsciiString> for Const {
    fn from(s: AsciiString) -> Self {
        Const::String(s)
    }
}

impl From<bool> for Const {
    fn from(v: bool) -> Self {
        if v { Const::True } else { Const::False }
    }
}

impl From<f64> for Const {
    fn from(f: f64) -> Self {
        Const::Float(f.into())
    }
}

impl From<i64> for Const {
    fn from(i: i64) -> Self {
        Const::Int(i)
    }
}

#[derive(Clone, Debug, Hash, Eq, PartialEq)]
pub(crate) enum Expr {
    /// __sil_allocate(\<ty\>)
    Alloc(Ty),
    /// foo(1, 2, 3)
    Call(FunctionName, Box<[Expr]>),
    /// 0, null, etc
    Const(Const),
    /// *Variable
    Deref(Box<Expr>),
    /// a.b
    Field(Box<Expr>, Ty, FieldName),
    /// a[b]
    Index(Box<Expr>, Box<Expr>),
    /// __sil_instanceof(expr, \<ty\>, nullable)
    InstanceOf(Box<Expr>, Ty, bool),
    Sid(Sid),
    Var(VarName),
}

impl Expr {
    pub(crate) fn call(target: FunctionName, params: impl VarArgs) -> Expr {
        Expr::Call(target, params.into_exprs().into_boxed_slice())
    }

    pub(crate) fn deref(v: impl Into<Expr>) -> Expr {
        Expr::Deref(Box::new(v.into()))
    }

    pub(crate) fn field(base: impl Into<Expr>, base_ty: Ty, name: FieldName) -> Expr {
        let base = base.into();
        Expr::Field(Box::new(base), base_ty, name)
    }

    #[allow(dead_code)]
    pub(crate) fn index(base: impl Into<Expr>, offset: impl Into<Expr>) -> Expr {
        let base = base.into();
        let offset = offset.into();
        Expr::Index(Box::new(base), Box::new(offset))
    }

    pub(crate) fn null() -> Expr {
        Expr::Const(Const::Null)
    }

    #[allow(clippy::todo)]
    pub(crate) fn ty(&self) -> Ty {
        match self {
            Expr::Sid(_) => Ty::mixed_ptr(),
            Expr::Const(Const::Null) => Ty::VoidPtr,
            _ => todo!("EXPR: {self:?}"),
        }
    }
}

impl<T: Into<Const>> From<T> for Expr {
    fn from(value: T) -> Self {
        Expr::Const(value.into())
    }
}

impl From<Sid> for Expr {
    fn from(sid: Sid) -> Self {
        Expr::Sid(sid)
    }
}

impl From<VarName> for Expr {
    fn from(var: VarName) -> Self {
        Expr::Var(var)
    }
}

impl From<LocalId> for Expr {
    fn from(lid: LocalId) -> Expr {
        Expr::Var(lid.into())
    }
}

pub(crate) trait VarArgs {
    fn into_exprs(self) -> Vec<Expr>;
}

impl<T> VarArgs for &[T]
where
    T: Into<Expr> + Copy,
{
    fn into_exprs(self) -> Vec<Expr> {
        self.iter().copied().map_into().collect_vec()
    }
}

impl VarArgs for () {
    fn into_exprs(self) -> Vec<Expr> {
        Vec::new()
    }
}

impl<T> VarArgs for [T; 1]
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        let [a] = self;
        vec![a.into()]
    }
}

impl<T> VarArgs for [T; 2]
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        let [a, b] = self;
        vec![a.into(), b.into()]
    }
}

impl<T> VarArgs for [T; 3]
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        let [a, b, c] = self;
        vec![a.into(), b.into(), c.into()]
    }
}

impl<T> VarArgs for [T; 4]
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        let [a, b, c, d] = self;
        vec![a.into(), b.into(), c.into(), d.into()]
    }
}

impl<T> VarArgs for Vec<T>
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        self.into_iter().map_into().collect_vec()
    }
}

impl<T> VarArgs for Box<[T]>
where
    T: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        Vec::from(self).into_exprs()
    }
}

impl<P1, P2> VarArgs for (P1, P2)
where
    P1: Into<Expr>,
    P2: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        vec![self.0.into(), self.1.into()]
    }
}

impl<P1, P2, P3> VarArgs for (P1, P2, P3)
where
    P1: Into<Expr>,
    P2: Into<Expr>,
    P3: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        vec![self.0.into(), self.1.into(), self.2.into()]
    }
}

impl<P1, P2, P3, P4> VarArgs for (P1, P2, P3, P4)
where
    P1: Into<Expr>,
    P2: Into<Expr>,
    P3: Into<Expr>,
    P4: Into<Expr>,
{
    fn into_exprs(self) -> Vec<Expr> {
        vec![self.0.into(), self.1.into(), self.2.into(), self.3.into()]
    }
}

impl<T, I, O, F> VarArgs for std::iter::Map<I, F>
where
    T: Into<Expr>,
    I: Iterator<Item = O>,
    F: FnMut(O) -> T,
{
    fn into_exprs(self) -> Vec<Expr> {
        self.map_into().collect_vec()
    }
}

pub(crate) enum FileAttribute {
    SourceLanguage(String),
}

pub(crate) struct FuncBuilder<'a, 'b> {
    cur_loc: Option<SrcLoc>,
    next_id: Sid,
    pub(crate) txf: &'a mut TextualFile<'b>,
    cache: Box<dyn VarCache>,
}

impl FuncBuilder<'_, '_> {
    pub fn alloc_sid(&mut self) -> Sid {
        let id = self.next_id;
        self.next_id = Sid::from_usize(id.as_usize() + 1);
        id
    }
}

impl FuncBuilder<'_, '_> {
    pub(crate) fn call(&mut self, target: &FunctionName, params: impl VarArgs) -> Result<Sid> {
        let params = params.into_exprs();
        let dst = self.alloc_sid();
        write!(self.txf.w, "{INDENT}{dst} = ", dst = FmtSid(dst),)?;
        self.write_call_expr(target, &params)?;
        writeln!(self.txf.w)?;
        Ok(dst)
    }

    /// Call the target as a static call (without virtual dispatch).
    pub(crate) fn call_static(
        &mut self,
        target: &FunctionName,
        this: Expr,
        params: impl VarArgs,
    ) -> Result<Sid> {
        self.txf.register_called_function(target);
        let dst = self.alloc_sid();
        write!(
            self.txf.w,
            "{INDENT}{dst} = {target}(",
            dst = FmtSid(dst),
            target = target.display()
        )?;
        self.write_expr(&this)?;
        let params = params.into_exprs();
        for param in params {
            self.txf.w.write_all(b", ")?;
            self.write_expr(&param)?;
        }
        writeln!(self.txf.w, ")")?;
        Ok(dst)
    }

    /// Call the target as a virtual call.
    pub(crate) fn call_virtual(
        &mut self,
        target: &FunctionName,
        this: Expr,
        params: impl VarArgs,
    ) -> Result<Sid> {
        self.txf.register_called_function(target);
        let dst = self.alloc_sid();
        write!(self.txf.w, "{INDENT}{dst} = ", dst = FmtSid(dst),)?;
        self.write_expr(&this)?;
        write!(self.txf.w, ".{}(", target.display())?;
        let params = params.into_exprs();
        let mut sep = "";
        for param in params {
            self.txf.w.write_all(sep.as_bytes())?;
            self.write_expr(&param)?;
            sep = ", ";
        }
        writeln!(self.txf.w, ")")?;
        Ok(dst)
    }

    pub(crate) fn comment(&mut self, msg: &str) -> Result<()> {
        writeln!(self.txf.w, "// {msg}")?;
        Ok(())
    }

    pub(crate) fn jmp(&mut self, targets: &[BlockId], params: impl VarArgs) -> Result {
        assert!(!targets.is_empty());

        let params = params.into_exprs();
        write!(self.txf.w, "{INDENT}jmp ")?;

        let mut sep1 = "";
        for &target in targets {
            write!(self.txf.w, "{sep1}{}", FmtBid(target))?;
            sep1 = ", ";

            if !params.is_empty() {
                write!(self.txf.w, "(")?;
                let mut sep2 = "";
                for param in params.iter() {
                    self.txf.w.write_all(sep2.as_bytes())?;
                    self.write_expr(param)?;
                    sep2 = ", ";
                }
                write!(self.txf.w, ")")?;
            }
        }

        writeln!(self.txf.w)?;

        Ok(())
    }

    pub(crate) fn lazy_class_initialize(&mut self, ty: &Ty) -> Result<Sid> {
        let dst = self.alloc_sid();
        writeln!(
            self.txf.w,
            "{INDENT}{dst} = __sil_lazy_class_initialize(<{ty}>)",
            dst = FmtSid(dst),
            ty = ty.display(),
        )?;
        Ok(dst)
    }

    pub(crate) fn load(&mut self, ty: &Ty, src: impl Into<Expr>) -> Result<Sid> {
        let src = src.into();
        // Technically the load should include the type because you could have
        // two loads of the same expression to two different types. I doubt that
        // it matters in reality.
        Ok(if let Some(sid) = self.cache.lookup(&src) {
            sid
        } else {
            let dst = self.alloc_sid();
            write!(
                self.txf.w,
                "{INDENT}{dst}: {ty} = load ",
                dst = FmtSid(dst),
                ty = ty.display(),
            )?;
            self.write_expr(&src)?;
            writeln!(self.txf.w)?;
            self.cache.load(&src, dst);
            dst
        })
    }

    // Terminate this branch if `expr` is false.
    pub(crate) fn prune(&mut self, expr: impl Into<Expr>) -> Result {
        let expr = expr.into();
        write!(self.txf.w, "{INDENT}prune ")?;
        self.write_expr(&expr)?;
        writeln!(self.txf.w)?;
        Ok(())
    }

    // Terminate this branch if `expr` is true.
    pub(crate) fn prune_not(&mut self, expr: impl Into<Expr>) -> Result {
        let expr = expr.into();
        write!(self.txf.w, "{INDENT}prune ! ")?;
        self.write_expr(&expr)?;
        writeln!(self.txf.w)?;
        Ok(())
    }

    pub(crate) fn ret(&mut self, expr: impl Into<Expr>, _kind: VerifyRetKind) -> Result {
        let expr = expr.into();
        write!(self.txf.w, "{INDENT}ret ")?;
        self.write_expr(&expr)?;
        writeln!(self.txf.w)?;
        Ok(())
    }

    pub(crate) fn store(
        &mut self,
        dst: impl Into<Expr>,
        src: impl Into<Expr>,
        src_ty: &Ty,
    ) -> Result {
        let dst = dst.into();
        let src = src.into();
        write!(self.txf.w, "{INDENT}store ")?;
        self.write_expr(&dst)?;
        self.txf.w.write_all(b" <- ")?;
        self.write_expr(&src)?;
        writeln!(self.txf.w, ": {ty}", ty = src_ty.display())?;
        self.cache.store(&dst, src);
        Ok(())
    }

    pub(crate) fn unreachable(&mut self) -> Result {
        writeln!(self.txf.w, "{INDENT}unreachable")?;
        Ok(())
    }

    pub(crate) fn throw(&mut self, exn_expr: impl Into<Expr>) -> Result {
        let exn_expr = exn_expr.into();
        write!(self.txf.w, "{INDENT}throw ",)?;
        self.write_expr(&exn_expr)?;
        writeln!(self.txf.w)?;
        Ok(())
    }

    /// A curry boxes up some parameters and returns an invokable.  This has to
    /// be an intrinsic so we don't end up a ton of little duplicate classes.
    ///
    /// It's usually used for function pointers or meth_callers:
    ///
    ///   `foo<>` turns into `AllocCurry("<$root>", "foo", null, [])`.
    ///   `C::foo<>` turns into `AllocCurry("<C$static>", "foo", static_this, [])`.
    ///   `$x->foo<>` turns into `AllocCurry("<C>", "foo", $x, [])`.
    ///
    /// Note that it's important that when the curry is invoked it replaces the
    /// callee's `this` with its own stored `this`.
    ///
    /// Curry can also be used for partial apply:
    ///
    ///   x = AllocCurry("<$root>", "foo", null, [1, 2])
    ///   x(3, 4)
    ///
    /// would be the same as:
    ///
    ///   foo(1, 2, 3, 4)
    ///
    /// If `this` is Some(_) then the curry will be a virtual call.
    pub(crate) fn write_alloc_curry(
        &mut self,
        name: FunctionName,
        this: Option<Expr>,
        args: impl VarArgs,
    ) -> Result<Sid> {
        let args = args.into_exprs();

        let curry_ty = CurryTy {
            name: name.clone(),
            virtual_call: this.is_some(),
            arg_tys: args.iter().map(|expr| expr.ty()).collect(),
        };
        let ty = curry_ty.ty();
        self.txf.curry_tys.insert(curry_ty);

        let obj = self.write_expr_stmt(Expr::Alloc(ty.clone()))?;

        if let Some(this) = this {
            let FunctionName::Method(captured_this_ty, _) = &name else {
                unreachable!();
            };
            self.store(
                Expr::Field(Box::new(obj.into()), ty.clone(), FieldName::raw("this")),
                this,
                &Ty::named_type_ptr(captured_this_ty.clone()),
            )?;
        }

        for (idx, arg) in args.into_iter().enumerate() {
            let field_ty = arg.ty();
            let field_name = FieldName::raw(format!("arg{idx}"));
            self.store(Expr::field(obj, ty.clone(), field_name), arg, &field_ty)?;
        }

        Ok(obj)
    }

    pub(crate) fn write_exception_handler(&mut self, target: BlockId) -> Result {
        writeln!(self.txf.w, "{INDENT}.handlers {}", FmtBid(target))?;
        Ok(())
    }

    pub(crate) fn write_expr_stmt(&mut self, expr: impl Into<Expr>) -> Result<Sid> {
        let expr = expr.into();
        match expr {
            Expr::Alloc(_)
            | Expr::InstanceOf(..)
            | Expr::Const(_)
            | Expr::Deref(_)
            | Expr::Field(..)
            | Expr::Index(..)
            | Expr::Var(..) => {
                let sid = self.alloc_sid();
                write!(self.txf.w, "{INDENT}{} = ", FmtSid(sid))?;
                self.write_expr(&expr)?;
                writeln!(self.txf.w)?;
                Ok(sid)
            }
            Expr::Call(target, params) => self.call(&target, params),
            Expr::Sid(sid) => Ok(sid),
        }
    }

    pub(crate) fn write_label(&mut self, bid: BlockId, params: &[Sid]) -> Result {
        self.cache.clear();
        if params.is_empty() {
            writeln!(self.txf.w, "#{}:", FmtBid(bid))?;
        } else {
            write!(self.txf.w, "#{}(", FmtBid(bid))?;
            let mut sep = "";
            for sid in params {
                write!(self.txf.w, "{sep}{}: *HackMixed", FmtSid(*sid))?;
                sep = ", ";
            }
            writeln!(self.txf.w, "):")?;
        }
        Ok(())
    }

    pub(crate) fn write_loc(&mut self, src_loc: &SrcLoc) -> Result {
        if self.cur_loc.as_ref().is_some() {
            self.txf.write_line_loc(src_loc)?;
            self.cur_loc = Some(src_loc.clone());
        } else {
            self.txf.write_full_loc(src_loc)?;
            self.cur_loc = Some(src_loc.clone());
        }
        Ok(())
    }
}

impl ExprWriter for FuncBuilder<'_, '_> {
    fn register_called_function(&mut self, target: &FunctionName) {
        self.txf.register_called_function(target);
    }

    fn register_used_param(&mut self, param: &Expr) {
        // Treat any call as if it can modify memory.
        self.cache.clear_expr(param);
    }

    fn register_used_global(&mut self, global: &GlobalName) {
        if !self.txf.referenced_globals.contains(global) {
            self.txf.referenced_globals.insert(global.to_owned());
        }
    }

    fn internal_get_writer(&mut self) -> &mut dyn std::io::Write {
        self.txf.internal_get_writer()
    }
}

trait ExprWriter {
    fn register_called_function(&mut self, target: &FunctionName);
    fn register_used_param(&mut self, param: &Expr);
    fn register_used_global(&mut self, global: &GlobalName);

    fn internal_get_writer(&mut self) -> &mut dyn std::io::Write;

    fn write_call_expr(&mut self, target: &FunctionName, params: &[Expr]) -> Result {
        self.register_called_function(target);
        write!(self.internal_get_writer(), "{}(", target.display())?;

        let mut sep = "";
        for param in params.iter() {
            self.internal_get_writer().write_all(sep.as_bytes())?;
            self.write_expr(param)?;
            sep = ", ";
        }

        write!(self.internal_get_writer(), ")")?;

        for param in params.iter() {
            self.register_used_param(param);
        }

        Ok(())
    }

    fn write_const(&mut self, value: &Const) -> Result {
        match value {
            Const::EnumClassLabel => {
                write!(self.internal_get_writer(), "$builtins.hack_enum_label()")?;
            }
            Const::False => self.internal_get_writer().write_all(b"false")?,
            Const::Float(d) => {
                let fun = match d.to_f64() {
                    d if d.is_finite() => {
                        write!(self.internal_get_writer(), "{:?}", d)?;
                        return Ok(());
                    }
                    d if d.is_infinite() && d.is_sign_negative() => hack::HackConst::NegInf,
                    d if d.is_infinite() && d.is_sign_positive() => hack::HackConst::Inf,
                    d if d.is_nan() => hack::HackConst::NaN,
                    _ => unreachable!(),
                };
                let fun = FunctionName::Builtin(hack::Builtin::HackConst(fun));
                self.register_called_function(&fun);
                self.write_call_expr(&fun, &[])?;
            }
            Const::Int(i) => write!(self.internal_get_writer(), "{i}")?,
            Const::LazyClass(s) => {
                write!(
                    self.internal_get_writer(),
                    "__sil_get_lazy_class(<{}>)",
                    s.display()
                )?;
            }
            Const::Null => self.internal_get_writer().write_all(b"null")?,
            Const::String(s) => {
                // String should already be escaped...
                write!(self.internal_get_writer(), "\"{s}\"")?;
            }
            Const::True => self.internal_get_writer().write_all(b"true")?,
        }
        Ok(())
    }

    fn write_expr(&mut self, expr: &Expr) -> Result {
        match *expr {
            Expr::Alloc(ref ty) => write!(
                self.internal_get_writer(),
                "__sil_allocate(<{}>)",
                ty.display()
            )?,
            Expr::Call(ref target, ref params) => {
                self.write_call_expr(target, params)?;
            }
            Expr::Const(ref c) => self.write_const(c)?,
            Expr::Deref(ref var) => {
                self.internal_get_writer().write_all(b"&")?;
                self.write_expr(var)?;
            }
            Expr::Field(ref base, ref ty, ref name) => {
                self.write_expr(base)?;
                write!(
                    self.internal_get_writer(),
                    ".{}.{}",
                    ty.display(),
                    name.display()
                )?;
            }
            Expr::Index(ref base, ref offset) => {
                self.write_expr(base)?;
                self.internal_get_writer().write_all(b"[")?;
                self.write_expr(offset)?;
                self.internal_get_writer().write_all(b"]")?;
            }
            Expr::InstanceOf(ref expr, ref ty, ref nullable) => {
                write!(self.internal_get_writer(), "__sil_instanceof(")?;
                self.write_expr(expr)?;
                write!(
                    self.internal_get_writer(),
                    ", <{}>, {})",
                    ty.display(),
                    if *nullable { "1" } else { "0" }
                )?;
            }
            Expr::Sid(sid) => write!(self.internal_get_writer(), "{}", FmtSid(sid))?,
            Expr::Var(ref var) => {
                match var {
                    VarName::Global(s) => {
                        self.register_used_global(s);
                    }
                    VarName::Local(_) => {}
                }
                write!(self.internal_get_writer(), "{}", var.display())?
            }
        }
        Ok(())
    }
}

trait VarCache {
    fn clear(&mut self);

    fn load(&mut self, src: &Expr, sid: Sid);

    fn clear_expr(&mut self, target: &Expr);

    fn store(&mut self, target: &Expr, value: Expr);

    fn lookup(&self, expr: &Expr) -> Option<Sid>;
}

#[derive(Default, Debug)]
struct NoVarCache;
impl VarCache for NoVarCache {
    fn clear(&mut self) {}

    fn load(&mut self, _src: &Expr, _sid: Sid) {}

    fn clear_expr(&mut self, _target: &Expr) {}

    fn store(&mut self, _target: &Expr, _value: Expr) {}

    fn lookup(&self, _expr: &Expr) -> Option<Sid> {
        None
    }
}

#[derive(Default, Debug)]
struct HashMapVarCache {
    cache: HashMap<VarName, Sid>,
}

impl VarCache for HashMapVarCache {
    fn clear(&mut self) {
        self.cache.clear();
    }

    fn load(&mut self, src: &Expr, sid: Sid) {
        match src {
            Expr::Deref(deref!(Expr::Var(var))) => {
                self.cache.insert(var.clone(), sid);
            }
            _ => {}
        }
    }

    fn clear_expr(&mut self, target: &Expr) {
        match target {
            Expr::Deref(deref!(Expr::Var(var))) => {
                self.cache.remove(var);
            }
            _ => {}
        }
    }

    fn store(&mut self, target: &Expr, value: Expr) {
        match target {
            Expr::Deref(deref!(Expr::Var(var))) => {
                // If we're storing into a local then clear it.
                match value {
                    Expr::Sid(sid) => {
                        // If we're storing a direct value then remember the
                        // value for later.
                        self.load(target, sid);
                    }
                    _ => {
                        self.cache.remove(var);
                    }
                }
            }
            _ => {}
        }
    }

    fn lookup(&self, expr: &Expr) -> Option<Sid> {
        match expr {
            Expr::Deref(deref!(Expr::Var(var))) => self.cache.get(var).cloned(),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash)]
pub(crate) enum Visibility {
    Public,
    Private,
    Protected,
}

impl Visibility {
    fn decl(&self) -> &str {
        match self {
            Visibility::Public => ".public",
            Visibility::Private => ".private",
            Visibility::Protected => ".protected",
        }
    }
}

pub(crate) enum FieldAttribute {
    Unparameterized {
        name: TypeName,
    },
    Parameterized {
        name: TypeName,
        parameters: Vec<String>,
    },
}

impl FieldAttribute {
    fn name(&self) -> &TypeName {
        match self {
            Self::Unparameterized { name } | Self::Parameterized { name, .. } => name,
        }
    }

    fn display<'a>(&'a self) -> impl fmt::Display + 'a {
        struct D<'a> {
            attr: &'a FieldAttribute,
        }
        impl fmt::Display for D<'_> {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, ".{}", self.attr.name().display())?;
                match self.attr {
                    FieldAttribute::Unparameterized { .. } => {}
                    FieldAttribute::Parameterized {
                        name: _,
                        parameters,
                    } => {
                        let mut i = parameters.iter();
                        let param = i.next().unwrap();
                        let escaped_param = str::replace(param, r#"""#, r#"\""#);
                        write!(f, "= \"{escaped_param}\"")?;
                        for param in i {
                            write!(f, ", \"{param}\"")?;
                        }
                        write!(f, " ")?;
                    }
                }
                Ok(())
            }
        }
        D { attr: self }
    }
}

pub(crate) struct Field<'a> {
    pub name: FieldName,
    pub ty: Cow<'a, Ty>,
    pub visibility: Visibility,
    pub attributes: Vec<FieldAttribute>,
    pub comments: Vec<String>,
}

#[derive(Clone)]
pub(crate) struct Param<'a> {
    pub name: VarName,
    pub attrs: Option<Box<[Cow<'a, str>]>>,
    pub ty: Ty,
}

#[derive(Clone)]
pub(crate) struct Return<'a> {
    pub attrs: Option<Box<[Cow<'a, str>]>>,
    pub ty: Ty,
}

impl Return<'_> {
    pub(crate) fn new(ty: Ty) -> Self {
        Return { attrs: None, ty }
    }
}
