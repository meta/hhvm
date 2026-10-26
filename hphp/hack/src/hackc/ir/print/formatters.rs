// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! Formatting Functions
//!
//! These functions are used to print the IR in a pseudo-assembly format.
//!
//! The functions in this file (as opposed to print.rs) print partial lines of
//! IR. As a result they mostly will be in the form:
//! ```
//! struct FmtThing { .. }
//! impl Display for FmtThing { .. }
//! ```

use std::fmt::Display;
use std::fmt::Formatter;
use std::fmt::Result;

use ir_core::BareThisOp;
use ir_core::CollectionType;
use ir_core::DictEntry;
use ir_core::IncDecOp;
use ir_core::InitPropOp;
use ir_core::IsTypeOp;
use ir_core::MOpMode;
use ir_core::ReadonlyOp;
use ir_core::SetOpOp;
use ir_core::SpecialClsRef;
use ir_core::SrcLoc;
use ir_core::TParamInfo;
use ir_core::UpperBound;
use ir_core::instr::HasLoc;
use ir_core::instr::Special;
use ir_core::*;

use crate::print::FuncContext;
use crate::util::FmtEscapedString;
use crate::util::FmtSep;

pub(crate) fn get_bit<T>(attr: &mut T, bit: T, s: &'static str) -> Option<&'static str>
where
    T: Copy
        + std::ops::BitAnd<T, Output = T>
        + std::ops::SubAssign
        + std::ops::BitAndAssign<T>
        + PartialEq<T>,
{
    if *attr & bit == bit {
        *attr -= bit;
        Some(s)
    } else {
        None
    }
}

pub(crate) fn flag_get_bit<T>(
    flag: bool,
    attr: &mut T,
    bit: T,
    s: &'static str,
) -> Option<&'static str>
where
    T: Copy
        + std::ops::BitAnd<T, Output = T>
        + std::ops::SubAssign
        + std::ops::BitAndAssign<T>
        + PartialEq<T>,
{
    if !flag {
        None
    } else if *attr & bit == bit {
        *attr -= bit;
        Some(s)
    } else {
        None
    }
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub(crate) enum AttrContext {
    Class,
    Constant,
    Function,
    Method,
    Property,
    Typedef,
}

pub(crate) struct FmtAttr(pub Attr, pub AttrContext);

impl Display for FmtAttr {
    fn fmt(&self, w: &mut Formatter<'_>) -> Result {
        let FmtAttr(mut attr, ctx) = *self;

        let is_class = ctx == AttrContext::Class;
        let is_func = ctx == AttrContext::Function;
        let is_func_or_method = is_func || ctx == AttrContext::Method;
        let is_property = ctx == AttrContext::Property;

        #[rustfmt::skip]
        FmtSep::new(
            "[",
            ", ",
            "]",
            [
                get_bit(&mut attr, Attr::AttrAbstract, "abstract"),
                get_bit(&mut attr, Attr::AttrBuiltin, "builtin"),
                get_bit(&mut attr, Attr::AttrDeepInit, "deep_init"),
                get_bit(&mut attr, Attr::AttrDynamicallyCallable, "dynamically_callable"),
                get_bit(&mut attr, Attr::AttrDynamicallyReferenced, "dynamically_referenced"),
                flag_get_bit(is_func_or_method, &mut attr, Attr::AttrEagerOnly, "eager_only"),
                get_bit(&mut attr, Attr::AttrEnumClass, "enum_class"),
                get_bit(&mut attr, Attr::AttrFinal, "final"),
                get_bit(&mut attr, Attr::AttrForbidDynamicProps, "no_dynamic_props"),
                get_bit(&mut attr, Attr::AttrInitialSatisfiesTC, "initial_satisfies_tc"),
                get_bit(&mut attr, Attr::AttrInterceptable, "interceptable"),
                get_bit(&mut attr, Attr::AttrInterface, "interface"),
                get_bit(&mut attr, Attr::AttrInternal, "internal"),
                flag_get_bit(is_class, &mut attr, Attr::AttrIsClosureClass, "is_closure_class"),
                get_bit(&mut attr, Attr::AttrIsConst, "const"),
                flag_get_bit(is_func, &mut attr, Attr::AttrIsMethCaller, "is_meth_caller"),
                get_bit(&mut attr, Attr::AttrIsReadonly, "readonly"),
                get_bit(&mut attr, Attr::AttrLSB, "lsb"),
                get_bit(&mut attr, Attr::AttrLateInit, "late_init"),
                flag_get_bit(
                    is_property,
                    &mut attr,
                    Attr::AttrNoBadRedeclare,
                    "no_bad_redeclare",
                ),
                get_bit(&mut attr, Attr::AttrNoImplicitNullable, "no_implicit_null"),
                get_bit(&mut attr, Attr::AttrNoInjection, "no_injection"),
                get_bit(&mut attr, Attr::AttrNoOverride, "no_override"),
                flag_get_bit(is_class, &mut attr, Attr::AttrNoReifiedInit, "no_reified_init"),
                get_bit(&mut attr, Attr::AttrPersistent, "persistent"),
                get_bit(&mut attr, Attr::AttrPrivate, "private"),
                get_bit(&mut attr, Attr::AttrProtected, "protected"),
                get_bit(&mut attr, Attr::AttrProvenanceSkipFrame, "provenance_skip_frame"),
                get_bit(&mut attr, Attr::AttrPublic, "public"),
                get_bit(&mut attr, Attr::AttrReadonlyReturn, "readonly_return"),
                get_bit(&mut attr, Attr::AttrSealed, "sealed"),
                get_bit(&mut attr, Attr::AttrSplatParam, "splat_param"),
                get_bit(&mut attr, Attr::AttrStatic, "static"),
                flag_get_bit(
                    is_class,
                    &mut attr,
                    Attr::AttrInStrictPackage,
                    "in_strict_package",
                ),
                get_bit(&mut attr, Attr::AttrSystemInitialValue, "system_initial_value"),
                get_bit(&mut attr, Attr::AttrTrait, "trait"),
                flag_get_bit(
                    is_func_or_method,
                    &mut attr,
                    Attr::AttrVariadicNamedParam,
                    "variadic_named_param",
                ),
                get_bit(&mut attr, Attr::AttrVariadicParam, "variadic_param"),
            ]
            .into_iter()
            .flatten(),
            |w, s| w.write_str(s),
        )
        .fmt(w)?;

        assert!(attr.is_empty(), "Unprocessed attr: {attr:?}");

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_context_dependent_attrs() {
        assert_eq!(
            FmtAttr(Attr::AttrNoReifiedInit, AttrContext::Class).to_string(),
            "[no_reified_init]"
        );
        assert_eq!(
            FmtAttr(Attr::AttrVariadicNamedParam, AttrContext::Function).to_string(),
            "[variadic_named_param]"
        );
        assert_eq!(
            FmtAttr(Attr::AttrVariadicNamedParam, AttrContext::Method).to_string(),
            "[variadic_named_param]"
        );
    }
}

pub(crate) struct FmtIdentifierIdList<'vec>(pub &'vec [FmtIdentifierId]);

impl Display for FmtIdentifierIdList<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtIdentifierIdList(vec) = *self;
        if vec.is_empty() {
            Ok(())
        } else {
            write!(
                f,
                " <{}> ",
                FmtSep::comma(vec.iter(), |w, id| { id.fmt(w) })
            )
        }
    }
}

pub(crate) struct FmtTParamInfo<'tpi>(pub &'tpi TParamInfo);

impl Display for FmtTParamInfo<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtTParamInfo(tpi) = *self;
        write!(
            f,
            "[{} {}]",
            FmtIdentifierId(tpi.name.as_bytes_id()),
            tpi.shadows_class_tparam
        )
    }
}

pub(crate) struct FmtTParamInfoVec<'tpis>(pub &'tpis [TParamInfo]);

impl Display for FmtTParamInfoVec<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtTParamInfoVec(vec) = *self;
        if vec.is_empty() {
            Ok(())
        } else {
            write!(
                f,
                " <{}> ",
                FmtSep::comma(vec.iter(), |w, tpi| { FmtTParamInfo(tpi).fmt(w) })
            )
        }
    }
}

pub(crate) struct FmtAttribute<'a>(pub &'a Attribute);

impl Display for FmtAttribute<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtAttribute(attr) = *self;
        FmtIdentifierId(attr.name.as_bytes_id()).fmt(f)?;
        if !attr.arguments.is_empty() {
            write!(
                f,
                "({})",
                FmtSep::comma(attr.arguments.iter(), |w, tv| { FmtTypedValue(tv).fmt(w) })
            )?;
        }
        Ok(())
    }
}

pub(crate) struct FmtBareThisOp(pub BareThisOp);

impl Display for FmtBareThisOp {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            BareThisOp::Notice => "notice",
            BareThisOp::NoNotice => "no_notice",
            BareThisOp::NeverNull => "never_null",
            _ => panic!("bad BareThisOp value"),
        };
        f.write_str(s)
    }
}

pub struct FmtBid<'a>(pub &'a Func, pub BlockId, /* verbose */ pub bool);

impl Display for FmtBid<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtBid(body, bid, verbose) = *self;
        if let Some(block) = body.repr.get_block(bid) {
            let pname = block.pname_hint.as_ref();
            if !verbose {
                if let Some(pname) = pname {
                    return write!(f, "b{}[\"{}\"]", bid.as_usize(), pname);
                }
            }
        }

        FmtRawBid(bid).fmt(f)
    }
}

pub(crate) struct FmtClassGetCMode(pub ClassGetCMode);

impl Display for FmtClassGetCMode {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            ClassGetCMode::Normal => "normal",
            ClassGetCMode::ExplicitConversion => "explicit_conversion",
            ClassGetCMode::UnsafeBackdoor => "unsafe_backdoor",
            ClassGetCMode::StrictIsolationBackdoor => "strict_isolation_backdoor",
            _ => panic!("bad ClassGetCMode value"),
        };
        f.write_str(s)
    }
}

pub(crate) struct FmtCollectionType(pub(crate) CollectionType);

impl Display for FmtCollectionType {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let kind = match self.0 {
            CollectionType::Vector => "vector",
            CollectionType::Map => "map",
            CollectionType::Set => "set",
            CollectionType::Pair => "pair",
            CollectionType::ImmVector => "imm_vector",
            CollectionType::ImmMap => "imm_map",
            CollectionType::ImmSet => "imm_set",
            _ => panic!("bad CollectionType value"),
        };
        f.write_str(kind)
    }
}

pub(crate) struct FmtSetOpOp(pub SetOpOp);

impl FmtSetOpOp {
    pub(crate) fn as_str(&self) -> &'static str {
        match self.0 {
            SetOpOp::AndEqual => "&=",
            SetOpOp::ConcatEqual => ".=",
            SetOpOp::DivEqual => "/=",
            SetOpOp::MinusEqual => "-=",
            SetOpOp::ModEqual => "%=",
            SetOpOp::MulEqual => "*=",
            SetOpOp::OrEqual => "|=",
            SetOpOp::PlusEqual => "+=",
            SetOpOp::PowEqual => "**=",
            SetOpOp::SlEqual => "<<=",
            SetOpOp::SrEqual => ">>=",
            SetOpOp::XorEqual => "^=",
            _ => panic!("invalid SetOpOp value"),
        }
    }
}

impl Display for FmtSetOpOp {
    fn fmt(&self, w: &mut Formatter<'_>) -> Result {
        w.write_str(self.as_str())
    }
}

struct FmtFloat(f64);

impl Display for FmtFloat {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        use std::num::FpCategory;
        let value = self.0;
        let s = match value.classify() {
            FpCategory::Nan => "nan",
            FpCategory::Infinite if value.is_sign_negative() => "-inf",
            FpCategory::Infinite => "inf",
            FpCategory::Zero if value.is_sign_negative() => "-0.0",
            FpCategory::Zero => "0.0",
            FpCategory::Subnormal | FpCategory::Normal => {
                let mut s = format!("{}", value);
                if !s.contains('.') {
                    s.push_str(".0");
                }
                return f.write_str(&s);
            }
        };
        f.write_str(s)
    }
}

pub(crate) struct FmtFuncParams<'a>(pub &'a Func);

impl Display for FmtFuncParams<'_> {
    fn fmt(&self, w: &mut Formatter<'_>) -> Result {
        let FmtFuncParams(func) = *self;
        write!(
            w,
            "({})",
            FmtSep::comma(&func.repr.params, |w, param| crate::print::print_param(
                w, func, param
            ))
        )
    }
}

pub(crate) struct FmtIdentifierStr<'a>(pub(crate) &'a str);

impl Display for FmtIdentifierStr<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        FmtIdentifier(self.0.as_bytes()).fmt(f)
    }
}

pub(crate) struct FmtIdentifier<'a>(pub(crate) &'a [u8]);

impl Display for FmtIdentifier<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtIdentifier(id) = *self;

        // Ideally we print identifiers as non-quoted strings - but if they
        // contain "strange" characters then we need to quote and escape them.
        //
        // Note that we don't need quotes when an identifier contains '\'
        // because that's not a "strange" character - we don't unescape unquoted
        // identifiers.
        //
        // We need to quote an empty string so it doesn't disappear.
        // Or if it contains non-ascii printable chars.
        // Or ':' (to disambiguate "Foo::Bar" from "Foo"::"Bar").
        // Or if it starts with a non-letter, number, backslash.
        let needs_quote = id
            .first()
            .is_none_or(|&c| !(c.is_ascii_alphabetic() || c == b'_' || c == b'\\' || c == b'$'))
            || id[1..]
                .iter()
                .any(|&c| !c.is_ascii_graphic() || c == b':' || c == b'#');
        if !needs_quote {
            if let Ok(string) = std::str::from_utf8(id) {
                return f.write_str(string);
            }
        }

        FmtEscapedString(id).fmt(f)
    }
}

pub(crate) struct FmtIdentifierId(pub BytesId);

impl Display for FmtIdentifierId {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtIdentifierId(id) = *self;
        FmtIdentifier(id.as_bytes()).fmt(f)
    }
}

pub(crate) struct FmtImmediate<'a>(pub(crate) &'a Immediate);

impl Display for FmtImmediate<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        match self.0 {
            Immediate::Dir => write!(f, "dir"),
            Immediate::EnumClassLabel(value) => {
                write!(f, "enum_class_label({})", FmtQuotedStringId(*value))
            }
            Immediate::File => write!(f, "file"),
            Immediate::FuncCred => write!(f, "func_cred"),
            Immediate::Method => write!(f, "method"),
            Immediate::Named(name) => write!(f, "constant({})", FmtIdentifier(name.as_bytes())),
            Immediate::NewCol(k) => write!(f, "new_col({:?})", k),
            Immediate::Bool(_)
            | Immediate::Float(_)
            | Immediate::Int(_)
            | Immediate::LazyClass(_)
            | Immediate::Null
            | Immediate::String(_)
            | Immediate::Uninit
            | Immediate::Vec(_)
            | Immediate::Dict(_)
            | Immediate::Keyset(_) => {
                let tv: TypedValue = self.0.clone().try_into().unwrap();
                FmtTypedValue(&tv).fmt(f)
            }
        }
    }
}

pub struct FmtVid<'a>(pub &'a Func, pub ValueId, /* verbose */ pub bool);

impl Display for FmtVid<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtVid(body, vid, verbose) = *self;
        match vid.full() {
            FullInstrId::Imm(cid) => {
                if verbose {
                    FmtRawVid(vid).fmt(f)
                } else {
                    FmtImmId(body, cid).fmt(f)
                }
            }
            FullInstrId::Instr(iid) => {
                if let Some(instr) = body.repr.get_instr(iid) {
                    if matches!(instr, Instr::Special(Special::Tombstone)) {
                        write!(f, "tombstone(%{})", iid.as_usize())
                    } else {
                        write!(f, "%{}", iid.as_usize())
                    }
                } else if iid == InstrId::NONE {
                    write!(f, "%none")
                } else {
                    write!(f, "unknown(%{})", iid.as_usize())
                }
            }
            FullInstrId::None => f.write_str("none"),
        }
    }
}

pub(crate) struct FmtDocComment<'a>(pub Option<&'a [u8]>);

impl Display for FmtDocComment<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        if let Some(doc) = self.0 {
            FmtEscapedString(doc).fmt(f)
        } else {
            f.write_str("N")
        }
    }
}

#[allow(dead_code)]
pub(crate) struct FmtIncDecOp(pub IncDecOp);

#[allow(dead_code)]
impl FmtIncDecOp {
    pub(crate) fn as_str(&self) -> &'static str {
        match self.0 {
            IncDecOp::PreInc => "pre_inc",
            IncDecOp::PostInc => "post_inc",
            IncDecOp::PreDec => "pre_dec",
            IncDecOp::PostDec => "post_dec",
            _ => panic!("bad IncDecOp value"),
        }
    }
}

impl Display for FmtIncDecOp {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        f.write_str(self.as_str())
    }
}

pub(crate) struct FmtInitPropOp(pub InitPropOp);

impl Display for FmtInitPropOp {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            InitPropOp::Static => "static",
            InitPropOp::NonStatic => "non_static",
            _ => panic!("bad InitPropOp value"),
        };
        f.write_str(s)
    }
}

pub struct FmtInstr<'a>(pub &'a Func, pub InstrId);

impl Display for FmtInstr<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtInstr(func, iid) = self;
        let instr = func.repr.get_instr(*iid).unwrap();
        let mut ctx = FuncContext {
            cur_loc: *func.repr.get_loc(instr.loc_id()).unwrap(),
            live_instrs: Default::default(),
            verbose: true,
        };
        crate::print::print_instr(f, &mut ctx, func, *iid, instr)?;
        Ok(())
    }
}

pub(crate) struct FmtIsTypeOp(pub IsTypeOp);

impl Display for FmtIsTypeOp {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            IsTypeOp::Null => "null",
            IsTypeOp::Bool => "bool",
            IsTypeOp::Int => "int",
            IsTypeOp::Dbl => "double",
            IsTypeOp::Str => "string",
            IsTypeOp::Obj => "object",
            IsTypeOp::Res => "resource",
            IsTypeOp::Scalar => "scalar",
            IsTypeOp::Keyset => "keyset",
            IsTypeOp::Dict => "dict",
            IsTypeOp::Vec => "vec",
            IsTypeOp::ArrLike => "array",
            IsTypeOp::ClsMeth => "clsmeth",
            IsTypeOp::Func => "func",
            IsTypeOp::LegacyArrLike => "legacy_array",
            IsTypeOp::Class => "class",
            _ => panic!("bad IsTypeOp value"),
        };
        f.write_str(s)
    }
}

pub struct FmtLid(pub LocalId);

impl Display for FmtLid {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtLid(ref lid) = *self;
        match *lid {
            LocalId::Unnamed(id) => write!(f, "@{}", id.as_usize()),
            LocalId::Named(var) => FmtIdentifierId(var.as_bytes()).fmt(f),
        }
    }
}

pub(crate) struct FmtLids<'a>(pub(crate) &'a [LocalId]);

impl Display for FmtLids<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtLids(slice) = *self;
        write!(
            f,
            "[{}]",
            FmtSep::comma(slice.iter(), |f, v| { FmtLid(*v).fmt(f) })
        )
    }
}

pub(crate) struct FmtImmId<'a>(pub(crate) &'a Func, pub(crate) ImmId);

impl Display for FmtImmId<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtImmId(body, cid) = *self;
        let imm = body.repr.imm(cid);
        FmtImmediate(imm).fmt(f)
    }
}

pub struct FmtLoc<'a>(pub &'a SrcLoc);

impl Display for FmtLoc<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        write!(
            f,
            "{}:{},{}:{}",
            self.0.line_begin, self.0.col_begin, self.0.line_end, self.0.col_end
        )?;

        Ok(())
    }
}

pub struct FmtFullLoc<'a>(pub &'a SrcLoc);

impl Display for FmtFullLoc<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtFullLoc(loc) = self;
        FmtLoc(loc).fmt(f)?;
        Ok(())
    }
}

pub struct FmtLocId<'a>(pub &'a Func, pub LocId);

impl Display for FmtLocId<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtLocId(func, loc_id) = *self;
        if let Some(loc) = func.repr.locs.get(loc_id) {
            FmtLoc(loc).fmt(f)
        } else {
            write!(f, "<unknown loc #{loc_id}>")
        }
    }
}

pub(crate) struct FmtMOpMode(pub MOpMode);

impl Display for FmtMOpMode {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            MOpMode::None => "",
            MOpMode::Warn => "warn",
            MOpMode::Define => "define",
            MOpMode::Unset => "unset",
            MOpMode::InOut => "inout",
            _ => panic!("bad MopMode value"),
        };
        f.write_str(s)
    }
}

pub(crate) struct FmtQuotedStr<'a>(pub(crate) &'a [u8]);

impl Display for FmtQuotedStr<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        FmtEscapedString(self.0).fmt(f)
    }
}

pub(crate) struct FmtQuotedStringId(pub(crate) BytesId);

impl Display for FmtQuotedStringId {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtQuotedStringId(id) = *self;
        FmtEscapedString(id.as_bytes()).fmt(f)
    }
}

pub struct FmtRawBid(pub BlockId);

impl Display for FmtRawBid {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtRawBid(bid) = *self;
        if bid == BlockId::NONE {
            f.write_str("none")
        } else {
            write!(f, "b{}", bid.as_usize())
        }
    }
}

pub struct FmtRawVid(pub ValueId);

impl Display for FmtRawVid {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        match self.0.full() {
            FullInstrId::Instr(iid) => {
                if iid == InstrId::NONE {
                    f.write_str("%none")
                } else {
                    write!(f, "%{}", iid.as_usize())
                }
            }
            FullInstrId::Imm(cid) => {
                if cid == ImmId::NONE {
                    f.write_str("#none")
                } else {
                    write!(f, "#{}", cid.as_usize())
                }
            }
            FullInstrId::None => f.write_str("none"),
        }
    }
}

pub(crate) struct FmtReadonly(pub(crate) ReadonlyOp);

impl Display for FmtReadonly {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        match self.0 {
            ReadonlyOp::Readonly => write!(f, "readonly"),
            ReadonlyOp::Mutable => write!(f, "mutable"),
            ReadonlyOp::Any => write!(f, "any"),
            ReadonlyOp::CheckROCOW => write!(f, "check_ro_cow"),
            ReadonlyOp::CheckMutROCOW => write!(f, "check_mut_ro_cow"),
            _ => panic!("bad ReadonlyOp value"),
        }
    }
}

pub(crate) struct FmtSpecialClsRef(pub(crate) SpecialClsRef);

impl Display for FmtSpecialClsRef {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let s = match self.0 {
            SpecialClsRef::LateBoundCls => "late_bound",
            SpecialClsRef::SelfCls => "self",
            SpecialClsRef::ParentCls => "parent",
            _ => panic!("bad SpecialClsRef value"),
        };
        f.write_str(s)
    }
}

pub(crate) struct FmtTParams<'a>(pub(crate) &'a [UpperBound]);

impl Display for FmtTParams<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtTParams(bounds) = self;
        if bounds.is_empty() {
            Ok(())
        } else {
            write!(
                f,
                // Since tparams end with '>' we need an extra space to make
                // sure we don't confuse with '>>'.
                "<{} >",
                FmtSep::comma(bounds.iter(), |f, UpperBound { name, bounds }| {
                    FmtIdentifierId(name.as_bytes_id()).fmt(f)?;
                    if !bounds.is_empty() {
                        write!(f, ": ")?;
                        let mut sep = "";
                        for t in bounds.iter() {
                            write!(f, "{sep}{}", FmtTypeInfo(t))?;
                            sep = " + ";
                        }
                    }
                    Ok(())
                })
            )
        }
    }
}

pub(crate) struct FmtTypedValue<'a>(pub &'a TypedValue);

impl Display for FmtTypedValue<'_> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtTypedValue(tv) = *self;
        match tv {
            TypedValue::Uninit => write!(f, "uninit"),
            TypedValue::Int(v) => {
                write!(f, "{}", v)
            }
            TypedValue::Bool(b) => f.write_str(if *b { "true" } else { "false" }),
            TypedValue::String(v) => FmtEscapedString(v.as_bytes()).fmt(f),
            TypedValue::Float(v) => FmtFloat(v.to_f64()).fmt(f),
            TypedValue::LazyClass(lit) => {
                write!(f, "lazy({})", FmtQuotedStringId(lit.as_bytes_id()))
            }
            TypedValue::Null => f.write_str("null"),
            TypedValue::EnumClassLabel(l) => {
                write!(f, "enum_class_label({})", FmtEscapedString(l.as_bytes()),)
            }
            TypedValue::Vec(values) => {
                write!(
                    f,
                    "vec[{}]",
                    FmtSep::comma(values.iter(), |f, v| { FmtTypedValue(v).fmt(f) })
                )
            }
            TypedValue::Keyset(values) => {
                write!(
                    f,
                    "keyset[{}]",
                    FmtSep::comma(values.iter(), |f, v| { FmtTypedValue(v).fmt(f) })
                )
            }
            TypedValue::Dict(values) => {
                write!(
                    f,
                    "dict[{}]",
                    FmtSep::comma(values.iter(), |f, DictEntry { key, value }| {
                        write!(f, "{} => {}", FmtTypedValue(key), FmtTypedValue(value))
                    })
                )
            }
        }
    }
}

pub(crate) struct FmtVisibility(pub Visibility);

impl Display for FmtVisibility {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let vis = match self.0 {
            Visibility::Private => "private",
            Visibility::Public => "public",
            Visibility::Protected => "protected",
            Visibility::Internal => "internal",
            Visibility::ProtectedInternal => "protected internal",
        };
        vis.fmt(f)
    }
}

pub(crate) struct FmtTypeInfo<'a>(pub &'a TypeInfo);

impl<'a> Display for FmtTypeInfo<'a> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let FmtTypeInfo(ti) = self;
        write!(f, "<")?;
        if let Some(id) = ti.user_type.into_option() {
            FmtQuotedStringId(id.as_bytes()).fmt(f)?;
        } else {
            f.write_str("N")?;
        }
        f.write_str(" ")?;

        if let Some(id) = ti.type_constraint.name.into_option() {
            FmtQuotedStringId(id.as_bytes()).fmt(f)?;
        } else {
            f.write_str("N")?;
        }
        f.write_str(" ")?;

        use TypeConstraintFlags as TCF;
        let mut mods = ti.type_constraint.flags;
        [
            get_bit(&mut mods, TCF::DisplayNullable, "display_nullable"),
            get_bit(&mut mods, TCF::Nullable, "nullable"),
            get_bit(&mut mods, TCF::Resolved, "resolved"),
            get_bit(&mut mods, TCF::Soft, "soft"),
            get_bit(&mut mods, TCF::TypeConstant, "type_constant"),
            get_bit(&mut mods, TCF::TypeVar, "type_var"),
            get_bit(&mut mods, TCF::UpperBound, "upper_bound"),
        ]
        .into_iter()
        .flatten()
        .try_for_each(|s| write!(f, " {s}"))?;
        assert!(mods == TCF::NoFlags, "MOD: {:?}", ti.type_constraint.flags);

        write!(f, ">")?;

        Ok(())
    }
}
