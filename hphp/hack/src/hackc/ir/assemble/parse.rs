// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::result::Result::Ok;

use anyhow::Result;
use ir_core::AsTypeStructExceptionKind;
use ir_core::Attr;
use ir_core::Attribute;
use ir_core::BareThisOp;
use ir_core::BlockId;
use ir_core::BytesId;
use ir_core::ClassGetCMode;
use ir_core::ClassName;
use ir_core::CollectionType;
use ir_core::ConstName;
use ir_core::Constant;
use ir_core::Constraint;
use ir_core::DefaultValue;
use ir_core::DictEntry;
use ir_core::FatalOp;
use ir_core::FloatBits;
use ir_core::FunctionName;
use ir_core::ImmId;
use ir_core::Immediate;
use ir_core::IncDecOp;
use ir_core::InitPropOp;
use ir_core::InstrId;
use ir_core::IsTypeOp;
use ir_core::MOpMode;
use ir_core::Maybe;
use ir_core::MethodName;
use ir_core::ModuleName;
use ir_core::OODeclExistsOp;
use ir_core::Param;
use ir_core::PropName;
use ir_core::ReadonlyOp;
use ir_core::SetOpOp;
use ir_core::SpecialClsRef;
use ir_core::SrcLoc;
use ir_core::StringId;
use ir_core::TParamInfo;
use ir_core::TypeConstraintFlags;
use ir_core::TypeInfo;
use ir_core::TypeStructEnforceKind;
use ir_core::TypeStructResolveOp;
use ir_core::TypedValue;
use ir_core::VerifyRetKind;
use ir_core::Visibility;
use parse_macro_ir::parse;

use crate::tokenizer::Token;
use crate::tokenizer::TokenLoc;
use crate::tokenizer::Tokenizer;
use crate::util::unescape;

pub(crate) fn is_block(id: &str) -> bool {
    id.starts_with('b') && id.as_bytes().get(1).is_some_and(u8::is_ascii_digit)
}

pub(crate) fn is_lid(id: &[u8]) -> bool {
    id.starts_with(b"$") || id.starts_with(b"@")
}

pub(crate) fn is_vid(id: &[u8]) -> bool {
    id.starts_with(b"%") || id.starts_with(b"#")
}

fn parse_array_key(tokenizer: &mut Tokenizer<'_>) -> Result<TypedValue> {
    let t = tokenizer.expect_any_token()?;
    Ok(match t {
        Token::Identifier(s, _) if s == "lazy" => {
            parse!(tokenizer, "(" <id:parse_class_name> ")");
            TypedValue::LazyClass(id)
        }
        Token::Identifier(s, _) if is_int(s.as_bytes()) => {
            let i = s.parse()?;
            TypedValue::Int(i)
        }
        Token::QuotedString(_, s, _) => TypedValue::String(ir_core::intern_bytes(unescape(&s)?)),
        _ => return Err(t.bail(format!("Expected string or int but got {t}"))),
    })
}

pub(crate) fn parse_attr(tokenizer: &mut Tokenizer<'_>) -> Result<Attr> {
    let mut attr = Attr::AttrNone;

    fn convert_attr(id: &str) -> Option<Attr> {
        Some(match id {
            "abstract" => Attr::AttrAbstract,
            "builtin" => Attr::AttrBuiltin,
            "is_closure_class" => Attr::AttrIsClosureClass,
            "const" => Attr::AttrIsConst,
            "deep_init" => Attr::AttrDeepInit,
            "dynamically_callable" => Attr::AttrDynamicallyCallable,
            "dynamically_referenced" => Attr::AttrDynamicallyReferenced,
            "eager_only" => Attr::AttrEagerOnly,
            "enum_class" => Attr::AttrEnumClass,
            "final" => Attr::AttrFinal,
            "initial_satisfies_tc" => Attr::AttrInitialSatisfiesTC,
            "interceptable" => Attr::AttrInterceptable,
            "interface" => Attr::AttrInterface,
            "internal" => Attr::AttrInternal,
            "is_meth_caller" => Attr::AttrIsMethCaller,
            "late_init" => Attr::AttrLateInit,
            "lsb" => Attr::AttrLSB,
            "no_bad_redeclare" => Attr::AttrNoBadRedeclare,
            "no_dynamic_props" => Attr::AttrForbidDynamicProps,
            "no_implicit_null" => Attr::AttrNoImplicitNullable,
            "no_injection" => Attr::AttrNoInjection,
            "no_override" => Attr::AttrNoOverride,
            "no_reified_init" => Attr::AttrNoReifiedInit,
            "persistent" => Attr::AttrPersistent,
            "private" => Attr::AttrPrivate,
            "protected" => Attr::AttrProtected,
            "provenance_skip_frame" => Attr::AttrProvenanceSkipFrame,
            "public" => Attr::AttrPublic,
            "readonly" => Attr::AttrIsReadonly,
            "readonly_return" => Attr::AttrReadonlyReturn,
            "sealed" => Attr::AttrSealed,
            "splat_param" => Attr::AttrSplatParam,
            "static" => Attr::AttrStatic,
            "in_strict_package" => Attr::AttrInStrictPackage,
            "system_initial_value" => Attr::AttrSystemInitialValue,
            "trait" => Attr::AttrTrait,
            "variadic_named_param" => Attr::AttrVariadicNamedParam,
            "variadic_param" => Attr::AttrVariadicParam,
            _ => return None,
        })
    }

    if tokenizer.next_is_identifier("[")? {
        parse!(tokenizer, <parts:id,*> "]");
        for id in parts {
            let bit = convert_attr(id.identifier())
                .ok_or_else(|| id.bail(format!("Unknown attr string {}", id)))?;
            attr.add(bit);
        }
    }
    Ok(attr)
}

pub(crate) fn parse_attribute(tokenizer: &mut Tokenizer<'_>) -> Result<Attribute> {
    let name = parse_class_name(tokenizer)?;
    let arguments = if tokenizer.next_is_identifier("(")? {
        parse!(tokenizer, <args:parse_typed_value,*> ")");
        args.into()
    } else {
        vec![].into()
    };
    Ok(Attribute { name, arguments })
}

pub(crate) fn parse_attributes(
    tokenizer: &mut Tokenizer<'_>,
    delimiter: &str,
) -> Result<Vec<Attribute>> {
    if tokenizer.next_is_identifier(delimiter)? {
        let attributes = parse_comma_list(tokenizer, false, parse_attribute)?;
        let close = match delimiter {
            "[" => "]",
            "<" => ">",
            _ => unreachable!(),
        };
        tokenizer.expect_identifier(close)?;
        Ok(attributes)
    } else {
        Ok(Vec::new())
    }
}

pub(crate) fn convert_bid(ident: &Token) -> Result<BlockId> {
    let s = ident.identifier();
    if s == "none" {
        return Ok(BlockId::NONE);
    }
    if !s.starts_with('b') {
        return Err(ident.bail(format!(
            "Expected BlockId but '{ident}' doesn't start with 'b'"
        )));
    }
    if let Ok(i) = s[1..].parse::<usize>() {
        Ok(BlockId::from_usize(i))
    } else {
        Err(ident.bail(format!("Error parsing number in BlockId '{ident}'")))
    }
}

pub(crate) fn parse_bid(tokenizer: &mut Tokenizer<'_>) -> Result<BlockId> {
    let ident = tokenizer.expect_any_identifier()?;
    convert_bid(&ident)
}

pub(crate) fn parse_class_name(tokenizer: &mut Tokenizer<'_>) -> Result<ClassName> {
    let (id, _) = parse_user_id(tokenizer)?;
    Ok(ClassName::from_utf8(&id)?)
}

pub(crate) fn parse_module_name(tokenizer: &mut Tokenizer<'_>) -> Result<ModuleName> {
    let (id, _) = parse_user_id(tokenizer)?;
    Ok(ModuleName::from_utf8(&id)?)
}

pub(crate) fn parse_const_name(tokenizer: &mut Tokenizer<'_>) -> Result<ConstName> {
    let (id, _) = parse_user_id(tokenizer)?;
    Ok(ConstName::from_utf8(&id)?)
}

pub(crate) fn parse_imm_id(tokenizer: &mut Tokenizer<'_>) -> Result<ImmId> {
    let t = tokenizer.expect_any_identifier()?;
    if !t.identifier().starts_with('#') {
        return Err(t.bail(format!("Expected '#' but got {t}")));
    }
    Ok(ImmId::from_usize(t.identifier()[1..].parse()?))
}

pub(crate) fn parse_comma_list<T, F: FnMut(&mut Tokenizer<'_>) -> Result<T>>(
    tokenizer: &mut Tokenizer<'_>,
    mut expect_comma: bool,
    mut callback: F,
) -> Result<Vec<T>> {
    // a, b, c
    let mut result = Vec::new();
    loop {
        if expect_comma {
            if !tokenizer.next_is_identifier(",")? {
                break;
            }
        } else {
            expect_comma = true;
        }

        result.push(callback(tokenizer)?);
    }

    Ok(result)
}

enum Num {
    Int(i64),
    Float(f64),
}

fn convert_num(t: &Token) -> Result<Num> {
    Ok(match t {
        Token::Identifier(s, _) if s == "inf" => Num::Float(f64::INFINITY),
        Token::Identifier(s, _) if s == "nan" => Num::Float(f64::NAN),
        Token::Identifier(s, _) if s.contains('.') => Num::Float(s.parse()?),
        Token::Identifier(s, _) => Num::Int(s.parse()?),
        _ => return Err(t.bail(format!("Expected number but got '{t}'"))),
    })
}

fn is_num_lead(c: u8) -> bool {
    c.is_ascii_digit() || c == b'-'
}

pub(crate) fn is_int(s: &[u8]) -> bool {
    if s.is_empty() {
        return false;
    }
    if s[0] == b'-' {
        is_int(&s[1..])
    } else {
        s.iter().all(|c| c.is_ascii_digit())
    }
}

pub(crate) fn parse_bare_this_op(tokenizer: &mut Tokenizer<'_>) -> Result<BareThisOp> {
    parse_enum(tokenizer, "BareThisOp", |t| {
        Some(match t {
            "notice" => BareThisOp::Notice,
            "no_notice" => BareThisOp::NoNotice,
            "never_null" => BareThisOp::NeverNull,
            _ => return None,
        })
    })
}

pub(crate) fn parse_class_get_c_kind(tokenizer: &mut Tokenizer<'_>) -> Result<ClassGetCMode> {
    parse_enum(tokenizer, "ClassGetCMode", |t| {
        Some(match t {
            "normal" => ClassGetCMode::Normal,
            "explicit_conversion" => ClassGetCMode::ExplicitConversion,
            "unsafe_backdoor" => ClassGetCMode::UnsafeBackdoor,
            "strict_isolation_backdoor" => ClassGetCMode::StrictIsolationBackdoor,
            _ => return None,
        })
    })
}

pub(crate) fn parse_imm(tokenizer: &mut Tokenizer<'_>) -> Result<Immediate> {
    Ok(match tokenizer.peek_token()? {
        Some(Token::QuotedString(..)) => {
            let t = tokenizer.expect_any_token()?;
            let id = ir_core::intern_bytes(t.unescaped_string()?);
            Immediate::String(id)
        }
        Some(Token::Identifier(s, _)) => match s.as_str() {
            "constant" => {
                tokenizer.expect_any_token()?;
                parse!(tokenizer, "(" <value:parse_user_id> ")");
                Immediate::Named(ConstName::intern(std::str::from_utf8(&value.0)?))
            }
            "dir" => {
                tokenizer.expect_any_token()?;
                Immediate::Dir
            }
            "file" => {
                tokenizer.expect_any_token()?;
                Immediate::File
            }
            "func_cred" => {
                tokenizer.expect_any_token()?;
                Immediate::FuncCred
            }
            "method" => {
                tokenizer.expect_any_token()?;
                Immediate::Method
            }
            "new_col" => {
                tokenizer.expect_any_token()?;
                parse!(tokenizer, "(" <kind:id> ")");
                let kind = match kind.identifier() {
                    "Vector" => CollectionType::Vector,
                    "Map" => CollectionType::Map,
                    "Set" => CollectionType::Set,
                    "Pair" => CollectionType::Pair,
                    "ImmVector" => CollectionType::ImmVector,
                    "ImmMap" => CollectionType::ImmMap,
                    "ImmSet" => CollectionType::ImmSet,
                    _ => return Err(kind.bail(format!("Unknown constant type '{kind}'"))),
                };
                Immediate::NewCol(kind)
            }
            "enum_class_label" => {
                tokenizer.expect_any_token()?;
                parse!(tokenizer, "(" <value:parse_string_id> ")");
                Immediate::EnumClassLabel(value)
            }
            _ => parse_typed_value(tokenizer)?.into(),
        },
        _ => parse_typed_value(tokenizer)?.into(),
    })
}

pub(crate) fn parse_doc_comment(tokenizer: &mut Tokenizer<'_>) -> Result<Option<Vec<u8>>> {
    Ok(if tokenizer.next_is_identifier("N")? {
        None
    } else {
        Some(tokenizer.expect_any_string()?.unescaped_string()?)
    })
}

pub(crate) fn parse_fatal_op(tokenizer: &mut Tokenizer<'_>) -> Result<FatalOp> {
    parse_enum(tokenizer, "FatalOp", |id| {
        Some(match id {
            "parse" => ir_core::FatalOp::Parse,
            "runtime" => ir_core::FatalOp::Runtime,
            "runtime_omit_frame" => ir_core::FatalOp::RuntimeOmitFrame,
            _ => return None,
        })
    })
}

pub(crate) fn parse_func_name(tokenizer: &mut Tokenizer<'_>) -> Result<FunctionName> {
    let (ident, _) = parse_user_id(tokenizer)?;
    Ok(FunctionName::from_utf8(&ident)?)
}

pub(crate) fn parse_hack_constant(tokenizer: &mut Tokenizer<'_>) -> Result<Constant> {
    parse!(tokenizer, <attrs:parse_attr> <name:parse_const_name>);

    let value = if tokenizer.next_is_identifier("=")? {
        Maybe::Just(parse_typed_value(tokenizer)?)
    } else {
        Maybe::Nothing
    };

    Ok(Constant { name, value, attrs })
}

fn parse_i32(tokenizer: &mut Tokenizer<'_>) -> Result<i32> {
    let t = tokenizer.expect_any_identifier()?;
    let i = t.identifier().parse()?;
    Ok(i)
}

pub(crate) fn parse_i64(tokenizer: &mut Tokenizer<'_>) -> Result<i64> {
    let t = tokenizer.expect_any_identifier()?;
    let i = t.identifier().parse()?;
    Ok(i)
}

pub(crate) fn parse_enum<T, F>(tokenizer: &mut Tokenizer<'_>, what: &str, f: F) -> Result<T>
where
    F: FnOnce(&str) -> Option<T>,
{
    let t = tokenizer.expect_any_identifier()?;
    if let Some(r) = f(t.identifier()) {
        Ok(r)
    } else {
        Err(t.bail(format!("Expected {what} but got '{t}'")))
    }
}

pub(crate) fn parse_inc_dec_op_post(
    tokenizer: &mut Tokenizer<'_>,
    op: Option<IncDecOp>,
) -> Result<IncDecOp> {
    Ok(match op {
        None => parse_enum(tokenizer, "IncDecOp", |t| {
            Some(match t {
                "++" => IncDecOp::PostInc,
                "--" => IncDecOp::PostDec,
                _ => return None,
            })
        })?,
        Some(op) => op,
    })
}

pub(crate) fn parse_inc_dec_op_pre(tokenizer: &mut Tokenizer<'_>) -> Result<Option<IncDecOp>> {
    parse_opt_enum(tokenizer, |t| {
        Some(match t {
            "++" => IncDecOp::PreInc,
            "--" => IncDecOp::PreDec,
            _ => return None,
        })
    })
}

pub(crate) fn parse_init_prop_op(tokenizer: &mut Tokenizer<'_>) -> Result<InitPropOp> {
    parse_enum(tokenizer, "InitPropOp", |t| {
        Some(match t {
            "static" => InitPropOp::Static,
            "non_static" => InitPropOp::NonStatic,
            _ => return None,
        })
    })
}

pub(crate) fn parse_instr_id(tokenizer: &mut Tokenizer<'_>) -> Result<InstrId> {
    let t = tokenizer.expect_any_identifier()?;
    if !t.identifier().starts_with('%') {
        return Err(t.bail(format!("Expected '%' but got {t}")));
    }
    Ok(InstrId::from_usize(t.identifier()[1..].parse()?))
}

pub(crate) fn parse_is_type_op(tokenizer: &mut Tokenizer<'_>) -> Result<IsTypeOp> {
    parse_enum(tokenizer, "IsTypeOp", |t| {
        Some(match t {
            "null" => IsTypeOp::Null,
            "bool" => IsTypeOp::Bool,
            "int" => IsTypeOp::Int,
            "double" => IsTypeOp::Dbl,
            "string" => IsTypeOp::Str,
            "object" => IsTypeOp::Obj,
            "resource" => IsTypeOp::Res,
            "scalar" => IsTypeOp::Scalar,
            "keyset" => IsTypeOp::Keyset,
            "dict" => IsTypeOp::Dict,
            "vec" => IsTypeOp::Vec,
            "array" => IsTypeOp::ArrLike,
            "clsmeth" => IsTypeOp::ClsMeth,
            "func" => IsTypeOp::Func,
            "legacy_array" => IsTypeOp::LegacyArrLike,
            "class" => IsTypeOp::Class,
            _ => return None,
        })
    })
}

pub(crate) fn parse_m_op_mode(tokenizer: &mut Tokenizer<'_>) -> Result<MOpMode> {
    Ok(parse_opt_enum(tokenizer, |id| {
        Some(match id {
            "warn" => MOpMode::Warn,
            "define" => MOpMode::Define,
            "unset" => MOpMode::Unset,
            "inout" => MOpMode::InOut,
            _ => return None,
        })
    })?
    .unwrap_or(MOpMode::None))
}

pub(crate) fn parse_method_name(tokenizer: &mut Tokenizer<'_>) -> Result<MethodName> {
    let (id, _) = parse_user_id(tokenizer)?;
    Ok(MethodName::from_utf8(&id)?)
}

pub(crate) fn parse_oo_decl_exists_op(tokenizer: &mut Tokenizer<'_>) -> Result<OODeclExistsOp> {
    parse_enum(tokenizer, "OODeclExistsOp", |id| {
        Some(match id {
            "class" => OODeclExistsOp::Class,
            "interface" => OODeclExistsOp::Interface,
            "trait" => OODeclExistsOp::Trait,
            _ => return None,
        })
    })
}

pub(crate) fn parse_opt_enum<T, F>(tokenizer: &mut Tokenizer<'_>, f: F) -> Result<Option<T>>
where
    F: FnOnce(&str) -> Option<T>,
{
    if let Some(t) = tokenizer.peek_if_any_identifier()? {
        if let Some(r) = f(t.identifier()) {
            tokenizer.read_token()?;
            return Ok(Some(r));
        }
    }

    Ok(None)
}

pub(crate) fn parse_param(tokenizer: &mut Tokenizer<'_>) -> Result<(Param, Option<DefaultValue>)> {
    parse!(tokenizer, <inout:"inout"?> <readonly:"readonly"?> <optional:"optional"?><named:"named"?><is_splat:"splat"?><user_attributes:parse_attributes("[")> <ty:parse_type_info>);

    let is_variadic = tokenizer.next_is_identifier("...")?;
    parse!(tokenizer, <name:parse_var>);

    let default_value = if tokenizer.next_is_identifier("@")? {
        parse!(tokenizer, <init:parse_bid> "(" <expr:string> ")");
        let expr = expr.unescaped_string()?;
        Some(DefaultValue { init, expr })
    } else {
        None
    };

    let param = Param {
        name,
        type_info: if ty.is_empty() {
            Maybe::Nothing
        } else {
            Maybe::Just(ty)
        },
        is_variadic,
        is_splat: is_splat.is_some(),
        is_inout: inout.is_some(),
        is_readonly: readonly.is_some(),
        is_optional: optional.is_some(),
        is_named: named.is_some(),
        user_attributes: user_attributes.into(),
    };
    Ok((param, default_value))
}

pub(crate) fn parse_prop_id(tokenizer: &mut Tokenizer<'_>) -> Result<PropName> {
    let (id, _) = parse_user_id(tokenizer)?;
    Ok(PropName::from_utf8(&id)?)
}

pub(crate) fn parse_readonly(tokenizer: &mut Tokenizer<'_>) -> Result<ReadonlyOp> {
    Ok(parse_opt_enum(tokenizer, |id| {
        Some(match id {
            "readonly" => ReadonlyOp::Readonly,
            "mutable" => ReadonlyOp::Mutable,
            "any" => ReadonlyOp::Any,
            "check_ro_cow" => ReadonlyOp::CheckROCOW,
            "check_mut_ro_cow" => ReadonlyOp::CheckMutROCOW,
            _ => return None,
        })
    })?
    .unwrap_or(ReadonlyOp::Any))
}

pub(crate) fn parse_single_tparam_info(tokenizer: &mut Tokenizer<'_>) -> Result<TParamInfo> {
    parse!(tokenizer, "[" <name:parse_class_name> <shadows_class_tparam:parse_bool> "]");
    Ok(TParamInfo {
        name,
        shadows_class_tparam,
    })
}

pub(crate) fn parse_tparam_info(tokenizer: &mut Tokenizer<'_>) -> Result<Vec<TParamInfo>> {
    Ok(if tokenizer.next_is_identifier("<")? {
        parse!(tokenizer, <elems:parse_single_tparam_info,*> ">");
        elems
    } else {
        Vec::new()
    })
}

pub(crate) fn parse_tparams(tokenizer: &mut Tokenizer<'_>) -> Result<Vec<ClassName>> {
    Ok(if tokenizer.next_is_identifier("<")? {
        parse!(tokenizer, <elems:parse_class_name,*> ">");
        elems
    } else {
        Vec::new()
    })
}

pub(crate) fn parse_special_cls_ref(tokenizer: &mut Tokenizer<'_>) -> Result<SpecialClsRef> {
    if let Some(t) = parse_special_cls_ref_opt(tokenizer)? {
        Ok(t)
    } else {
        parse_enum(tokenizer, "SpecialClsRef", |_| None)
    }
}

pub(crate) fn parse_special_cls_ref_opt(
    tokenizer: &mut Tokenizer<'_>,
) -> Result<Option<SpecialClsRef>> {
    parse_opt_enum(tokenizer, |id| {
        Some(match id {
            "late_bound" => SpecialClsRef::LateBoundCls,
            "self" => SpecialClsRef::SelfCls,
            "parent" => SpecialClsRef::ParentCls,
            _ => return None,
        })
    })
}

pub(crate) fn parse_src_loc(tokenizer: &mut Tokenizer<'_>) -> Result<SrcLoc> {
    parse!(tokenizer, <line_begin:parse_i32> ":" <col_begin:parse_i32> "," <line_end:parse_i32> ":" <col_end:parse_i32>);

    Ok(SrcLoc {
        line_begin,
        col_begin,
        line_end,
        col_end,
    })
}

pub(crate) fn parse_set_op_op(tokenizer: &mut Tokenizer<'_>) -> Result<SetOpOp> {
    parse_enum(tokenizer, "SetOpOp", |id| {
        Some(match id {
            "&=" => SetOpOp::AndEqual,
            ".=" => SetOpOp::ConcatEqual,
            "/=" => SetOpOp::DivEqual,
            "-=" => SetOpOp::MinusEqual,
            "%=" => SetOpOp::ModEqual,
            "*=" => SetOpOp::MulEqual,
            "|=" => SetOpOp::OrEqual,
            "+=" => SetOpOp::PlusEqual,
            "**=" => SetOpOp::PowEqual,
            "<<=" => SetOpOp::SlEqual,
            ">>=" => SetOpOp::SrEqual,
            "^=" => SetOpOp::XorEqual,
            _ => return None,
        })
    })
}

pub(crate) fn parse_string_id(tokenizer: &mut Tokenizer<'_>) -> Result<BytesId> {
    let name = tokenizer.expect_any_string()?;
    let name = name.unescaped_string()?;
    Ok(ir_core::intern_bytes(name))
}

pub(crate) fn parse_type_info(tokenizer: &mut Tokenizer<'_>) -> Result<TypeInfo> {
    parse!(tokenizer, "<" <user_type:tok> <name:tok> <flags:parse_type_constraint_flags> ">");

    let user_type = if user_type.is_identifier("N") {
        None
    } else if !user_type.is_any_string() {
        return Err(user_type.bail(format!("String expected but got '{user_type}'")));
    } else {
        Some(ir_core::intern(std::str::from_utf8(
            &user_type.unescaped_string()?,
        )?))
    };

    let name = if name.is_identifier("N") {
        None
    } else if !name.is_any_string() {
        return Err(name.bail(format!("String expected but got '{name}'")));
    } else {
        Some(ir_core::intern(std::str::from_utf8(
            &name.unescaped_string()?,
        )?))
    };

    Ok(TypeInfo {
        user_type: user_type.into(),
        type_constraint: Constraint {
            name: name.into(),
            flags,
        },
    })
}

fn parse_type_constraint_flags(tokenizer: &mut Tokenizer<'_>) -> Result<TypeConstraintFlags> {
    let mut flags = TypeConstraintFlags::NoFlags;
    while let Some(flag) = parse_type_constraint_flag(tokenizer)? {
        flags = flags | flag;
    }
    Ok(flags)
}

fn parse_type_constraint_flag(
    tokenizer: &mut Tokenizer<'_>,
) -> Result<Option<TypeConstraintFlags>> {
    parse_opt_enum(tokenizer, |id| {
        Some(match id {
            "nullable" => TypeConstraintFlags::Nullable,
            "type_var" => TypeConstraintFlags::TypeVar,
            "soft" => TypeConstraintFlags::Soft,
            "type_constant" => TypeConstraintFlags::TypeConstant,
            "resolved" => TypeConstraintFlags::Resolved,
            "display_nullable" => TypeConstraintFlags::DisplayNullable,
            "upper_bound" => TypeConstraintFlags::UpperBound,
            _ => return None,
        })
    })
}

pub(crate) fn parse_type_struct_resolve_op(
    tokenizer: &mut Tokenizer<'_>,
) -> Result<TypeStructResolveOp> {
    parse_enum(tokenizer, "TypeStructResolveOp", |id| {
        Some(match id {
            "resolve" => TypeStructResolveOp::Resolve,
            "dont_resolve" => TypeStructResolveOp::DontResolve,
            _ => return None,
        })
    })
}

pub(crate) fn parse_type_struct_enforce_kind(
    tokenizer: &mut Tokenizer<'_>,
) -> Result<TypeStructEnforceKind> {
    parse_enum(tokenizer, "TypeStructEnforceKind", |id| {
        Some(match id {
            "deep" => TypeStructEnforceKind::Deep,
            "shallow" => TypeStructEnforceKind::Shallow,
            _ => return None,
        })
    })
}

pub(crate) fn parse_as_type_struct_exception_kind(
    tokenizer: &mut Tokenizer<'_>,
) -> Result<AsTypeStructExceptionKind> {
    parse_enum(tokenizer, "AsTypeStructExceptionKind", |id| {
        Some(match id {
            "error" => AsTypeStructExceptionKind::Error,
            "typehint" => AsTypeStructExceptionKind::Typehint,
            _ => return None,
        })
    })
}
pub(crate) fn parse_typed_value(tokenizer: &mut Tokenizer<'_>) -> Result<TypedValue> {
    let t = tokenizer.expect_any_token()?;
    Ok(match t {
        Token::Identifier(s, _) if s == "dict" => {
            fn parse_arrow_tuple(
                tokenizer: &mut Tokenizer<'_>,
            ) -> Result<(TypedValue, TypedValue)> {
                parse!(tokenizer, <k:parse_array_key> "=>" <v:parse_typed_value>);
                Ok((k, v))
            }
            parse!(tokenizer, "[" <values:parse_arrow_tuple,*> "]");
            TypedValue::dict(
                values
                    .into_iter()
                    .map(|(key, value)| DictEntry { key, value })
                    .collect(),
            )
        }
        Token::Identifier(s, _) if s == "false" => TypedValue::Bool(false),
        Token::Identifier(s, _) if s == "inf" => TypedValue::Float(FloatBits(f64::INFINITY)),
        Token::Identifier(s, _) if s == "keyset" => {
            parse!(tokenizer, "[" <values:parse_array_key,*> "]");
            TypedValue::keyset(values)
        }
        Token::Identifier(s, _) if s == "lazy" => {
            parse!(tokenizer, "(" <id:parse_class_name> ")");
            TypedValue::LazyClass(id)
        }
        Token::Identifier(s, _) if s == "enum_class_label" => {
            parse!(tokenizer, "(" <id:parse_string_id> ")");
            TypedValue::EnumClassLabel(id)
        }
        Token::Identifier(s, _) if s == "nan" => TypedValue::Float(FloatBits(f64::NAN)),
        Token::Identifier(s, _) if s == "null" => TypedValue::Null,
        Token::Identifier(s, _) if s == "true" => TypedValue::Bool(true),
        Token::Identifier(s, _) if s == "uninit" => TypedValue::Uninit,
        Token::Identifier(s, _) if s == "vec" => {
            parse!(tokenizer, "[" <values:parse_typed_value,*> "]");
            TypedValue::vec(values)
        }
        Token::Identifier(s, _) if s == "-" && tokenizer.next_is_identifier("inf")? => {
            TypedValue::Float(FloatBits(f64::NEG_INFINITY))
        }
        Token::Identifier(ref s, _) if is_num_lead(s.as_bytes()[0]) => match convert_num(&t)? {
            Num::Int(i) => TypedValue::Int(i),
            Num::Float(f) => TypedValue::Float(FloatBits(f)),
        },
        Token::QuotedString(_, s, _) => {
            let id = ir_core::intern_bytes(unescape(&s)?);
            TypedValue::String(id)
        }
        _ => return Err(t.bail(format!("Invalid TypedValue, got '{t}'"))),
    })
}

pub(crate) fn parse_u32(tokenizer: &mut Tokenizer<'_>) -> Result<u32> {
    let t = tokenizer.expect_any_identifier()?;
    let i = t.identifier().parse()?;
    Ok(i)
}

pub(crate) fn parse_usize(tokenizer: &mut Tokenizer<'_>) -> Result<usize> {
    let t = tokenizer.expect_any_identifier()?;
    let i = t.identifier().parse()?;
    Ok(i)
}

pub(crate) fn parse_bool(tokenizer: &mut Tokenizer<'_>) -> Result<bool> {
    let t = tokenizer.expect_any_identifier()?;
    let b = t.identifier().parse()?;
    Ok(b)
}

pub(crate) fn parse_user_id(tokenizer: &mut Tokenizer<'_>) -> Result<(Vec<u8>, TokenLoc)> {
    let t = tokenizer.expect_any_token()?;
    let tloc = t.loc().clone();
    let value = t.unescaped_identifier()?;
    Ok((value.into_owned(), tloc))
}

fn parse_var(tokenizer: &mut Tokenizer<'_>) -> Result<StringId> {
    parse!(tokenizer, @pos <name:parse_user_id>);
    if name.0.first().copied() == Some(b'$') {
        Ok(ir_core::intern(std::str::from_utf8(&name.0)?))
    } else {
        Err(pos.bail(format!(
            "Expected leading '$' but got {}",
            &String::from_utf8_lossy(&name.0),
        )))
    }
}

pub(crate) fn parse_visibility(tokenizer: &mut Tokenizer<'_>) -> Result<Visibility> {
    let vis1 = parse_enum(tokenizer, "Visibility", str_to_visibility_fn())?;
    let vis2 = parse_opt_enum(tokenizer, str_to_visibility_fn())?;
    let vis = match (vis1, vis2) {
        (Visibility::Protected, Some(Visibility::Internal)) => Visibility::ProtectedInternal,
        (Visibility::Internal, Some(Visibility::Protected)) => Visibility::ProtectedInternal,
        _ => vis1,
    };
    Ok(vis)
}

pub(crate) fn parse_verify_kind(tokenizer: &mut Tokenizer<'_>) -> Result<VerifyRetKind> {
    parse_enum(tokenizer, "VerifyRetKind", |id| {
        Some(match id {
            "none" => VerifyRetKind::None,
            "all" => VerifyRetKind::All,
            "nonnull" => VerifyRetKind::NonNull,
            _ => return None,
        })
    })
}

fn str_to_visibility_fn() -> impl FnOnce(&str) -> Option<Visibility> {
    |id| {
        Some(match id {
            "private" => Visibility::Private,
            "public" => Visibility::Public,
            "protected" => Visibility::Protected,
            "internal" => Visibility::Internal,
            _ => return None,
        })
    }
}
