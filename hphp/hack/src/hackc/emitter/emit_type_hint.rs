// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::borrow::Cow;
use std::iter::once;

use error::Error;
use error::Result;
use ffi::Maybe;
use ffi::Maybe::*;
use hhbc::ClassName;
use hhbc::Constraint;
use hhbc::StringId;
use hhbc::TypeInfo;
use hhbc::string_id;
use hhbc_string_utils as string_utils;
use hhvm_types_ffi::ffi::TypeConstraintFlags;
use naming_special_names_rust::classes;
use naming_special_names_rust::typehints;
use oxidized::aast_defs::ClassPtrKind;
use oxidized::aast_defs::HfParamInfo;
use oxidized::aast_defs::Hint;
use oxidized::aast_defs::Hint_;
use oxidized::aast_defs::Hint_::*;
use oxidized::aast_defs::HintFun;
use oxidized::aast_defs::HintTparam;
use oxidized::aast_defs::NastShapeInfo;
use oxidized::aast_defs::ShapeElement;
use oxidized::aast_defs::ShapeFieldInfo;
use oxidized::aast_defs::Tprim;
use oxidized::aast_defs::TupleExtra;
use oxidized::aast_defs::TupleInfo;
use oxidized::ast_defs::ConstraintKind;
use oxidized::ast_defs::Id;
use oxidized::ast_defs::ParamKind;
use oxidized::ast_defs::ShapeFieldName;

#[derive(Eq, PartialEq)]
pub enum Kind {
    Property,
    Return,
    Param,
    TypeDef,
    UpperBound,
}

fn fmt_name_or_prim<'n>(tparams: &[&str], name: &'n str) -> Cow<'n, str> {
    if tparams.contains(&name) {
        name.into()
    } else {
        let id = ClassName::from_ast_name_and_mangle(name);
        if string_utils::is_xhp(string_utils::strip_ns(name)) {
            id.unmangled().into()
        } else {
            id.as_str().into()
        }
    }
}

pub fn prim_to_string(prim: &Tprim) -> &'static str {
    match prim {
        Tprim::Tnull => typehints::NULL,
        Tprim::Tvoid => typehints::VOID,
        Tprim::Tint => typehints::INT,
        Tprim::Tbool => typehints::BOOL,
        Tprim::Tfloat => typehints::FLOAT,
        Tprim::Tresource => typehints::RESOURCE,
        Tprim::Tnum => typehints::NUM,
        Tprim::Tarraykey => typehints::ARRAYKEY,
        Tprim::Tnoreturn => typehints::NORETURN,
    }
}

pub fn fmt_hint(tparams: &[&str], strip_tparams: bool, hint: &Hint) -> Result<String> {
    let Hint(_, h) = hint;
    Ok(match h.as_ref() {
        Habstr(id) => {
            let name = fmt_name_or_prim(tparams, id);

            name.to_string()
        }
        Happly(Id(_, id), args) => {
            let name = fmt_name_or_prim(tparams, id);
            if args.is_empty() || strip_tparams {
                name.to_string()
            } else {
                format!("{}<{}>", name, fmt_hints(tparams, args, ", ")?)
            }
        }
        HclassPtr(kind, hint) => {
            let k = match kind {
                ClassPtrKind::CKclass => "class",
                ClassPtrKind::CKenum => "enum",
            };
            format!("{}<{}>", k, fmt_hint(tparams, false, hint)?)
        }
        Hwildcard => "_".into(),
        Hfun(hf) => fmt_fun_hint(tparams, hf)?,
        Haccess(Hint(_, hint), accesses) => {
            let receiver = match hint.as_ref() {
                Happly(Id(_, id), _) => fmt_name_or_prim(tparams, id),
                Hvar(name) => Cow::Borrowed(name.as_str()),
                _ => {
                    return Err(Error::unrecoverable(
                        "ast_to_nast error. Expected a type or variable context receiver",
                    ));
                }
            };
            format!(
                "{}::{}",
                receiver,
                accesses
                    .iter()
                    .map(|Id(_, s)| s.as_str())
                    .collect::<Vec<_>>()
                    .join("::")
            )
        }
        Hoption(hint) => {
            let Hint(_, h) = hint;
            if let Hsoft(t) = h.as_ref() {
                // Follow HHVM order: soft -> option
                // Can we fix this eventually?
                format!("@?{}", fmt_hint(tparams, false, t)?)
            } else {
                format!("?{}", fmt_hint(tparams, false, hint)?)
            }
        }
        Hrefinement(hint, _) => {
            // NOTE: refinements are already banned in type structures
            // and in other cases they should be invisible to the HHVM, so unpack hint
            fmt_hint(tparams, strip_tparams, hint)?
        }
        Hshape(NastShapeInfo {
            allows_unknown_fields,
            field_map,
            unknown_fields_type,
        }) => {
            let fmt_field_name = |name: &ShapeFieldName| match name {
                ShapeFieldName::SFlitStr((_, s)) => format!("'{}'", s),
                ShapeFieldName::SFclassname(Id(_, cid)) => {
                    format!("'{}'", fmt_name_or_prim(tparams, cid))
                }
                ShapeFieldName::SFclassConst(Id(_, cid), (_, s)) => {
                    format!("{}::{}", fmt_name_or_prim(tparams, cid), s)
                }
            };
            let fmt_field = |field: &ShapeFieldInfo| {
                let prefix = if field.optional { "?" } else { "" };
                Ok(format!(
                    "{}{}=>{}",
                    prefix,
                    fmt_field_name(&field.name),
                    fmt_hint(tparams, false, &field.hint)?
                ))
            };
            let mut shape_fields = field_map
                .iter()
                .map(|elem| match elem {
                    ShapeElement::SEField(sfi) => fmt_field(sfi),
                    ShapeElement::SESplat(hint) => {
                        Ok(format!("...{}", fmt_hint(tparams, false, hint)?))
                    }
                })
                .collect::<Result<Vec<_>>>()?;
            if *allows_unknown_fields {
                shape_fields.push(match unknown_fields_type {
                    Some(hint) => format!("{}...", fmt_hint(tparams, false, hint)?),
                    None => "...".into(),
                });
            }
            string_utils::prefix_namespace("HH", &format!("shape({})", shape_fields.join(", ")))
        }
        Htuple(tuple) => fmt_tuple_hint(tparams, tuple)?,
        Hunion(hints) => format!("({})", fmt_hints(tparams, hints, " | ")?),
        Hintersection(hints) => format!("({})", fmt_hints(tparams, hints, " & ")?),
        Hlike(t) => format!("~{}", fmt_hint(tparams, false, t)?),
        Hsoft(t) => format!("@{}", fmt_hint(tparams, false, t)?),
        HfunContext(name) => format!("ctx {name}"),
        Hvar(name) => name.clone(),
        HvecOrDict(key, value) => {
            let value = fmt_hint(tparams, false, value)?;
            let arguments = match key {
                Some(key) => format!("{}, {value}", fmt_hint(tparams, false, key)?),
                None => value,
            };
            string_utils::prefix_namespace("HH", &format!("vec_or_dict<{arguments}>"))
        }
        Hdynamic | Hmixed | Hnonnull | Hnothing | Hprim(_) | Hthis => {
            fmt_name_or_prim(tparams, hint_to_string(h)).into()
        }
    })
}

fn hint_to_string<'a>(h: &'a Hint_) -> &'a str {
    match h {
        Hprim(p) => prim_to_string(p),
        Hmixed | Hunion(_) | Hintersection(_) => typehints::MIXED,
        Hnonnull => typehints::NONNULL,
        Hthis => typehints::THIS,
        Hdynamic => typehints::DYNAMIC,
        Hnothing => typehints::NOTHING,
        Habstr(_)
        | Haccess(_, _)
        | Happly(_, _)
        | HclassPtr(_, _)
        | Hfun(_)
        | HfunContext(_)
        | Hlike(_)
        | Hoption(_)
        | Hrefinement(_, _)
        | Hshape(_)
        | Hsoft(_)
        | Htuple(_)
        | Hvar(_)
        | HvecOrDict(_, _)
        | Hwildcard => {
            panic!("shouldn't invoke this function")
        }
    }
}

fn fmt_hints(tparams: &[&str], hints: &[Hint], separator: &str) -> Result<String> {
    hints
        .iter()
        .map(|h| fmt_hint(tparams, false, h))
        .collect::<Result<Vec<_>>>()
        .map(|v| v.join(separator))
}

fn fmt_tuple_hint(tparams: &[&str], tuple: &TupleInfo) -> Result<String> {
    let required = tuple
        .required
        .iter()
        .map(|hint| fmt_hint(tparams, false, hint));
    let elements = match &tuple.extra {
        TupleExtra::Hextra(extra) => required
            .chain(
                extra
                    .optional
                    .iter()
                    .map(|hint| Ok(format!("optional {}", fmt_hint(tparams, false, hint)?))),
            )
            .chain(
                extra
                    .variadic
                    .iter()
                    .map(|hint| Ok(format!("{}...", fmt_hint(tparams, false, hint)?))),
            )
            .collect::<Result<Vec<_>>>()?,
        TupleExtra::Hsplat(hint) => required
            .chain(once(Ok(format!("...{}", fmt_hint(tparams, false, hint)?))))
            .collect::<Result<Vec<_>>>()?,
    };
    Ok(format!("({})", elements.join(", ")))
}

fn fmt_fun_param(tparams: &[&str], hint: &Hint, info: Option<&HfParamInfo>) -> Result<String> {
    let hint = fmt_hint(tparams, false, hint)?;
    let Some(info) = info else {
        return Ok(hint);
    };
    let optional = if info.optional.is_some() {
        "optional "
    } else {
        ""
    };
    let inout = if matches!(info.kind, ParamKind::Pinout(_)) {
        "inout "
    } else {
        ""
    };
    let named = if info.named.is_some() { "named " } else { "" };
    let readonly = if info.readonlyness.is_some() {
        "readonly "
    } else {
        ""
    };
    let splat = if info.splat.is_some() { "..." } else { "" };
    let name = info
        .named
        .as_ref()
        .map(|name| format!(" {name}"))
        .unwrap_or_default();
    Ok(format!(
        "{optional}{inout}{named}{readonly}{splat}{hint}{name}"
    ))
}

fn fmt_fun_tparam(tparams: &[&str], tparam: &HintTparam) -> Result<String> {
    let attributes = if tparam.user_attributes.is_empty() {
        String::new()
    } else {
        format!(
            "<<{}>> ",
            tparam
                .user_attributes
                .iter()
                .map(|Id(_, name)| fmt_name_or_prim(tparams, name))
                .collect::<Vec<_>>()
                .join(", ")
        )
    };
    let constraints = tparam
        .constraints
        .iter()
        .map(|(kind, hint)| {
            let kind = match kind {
                ConstraintKind::ConstraintAs => "as",
                ConstraintKind::ConstraintEq => "=",
                ConstraintKind::ConstraintSuper => "super",
            };
            Ok(format!(" {kind} {}", fmt_hint(tparams, false, hint)?))
        })
        .collect::<Result<Vec<_>>>()?
        .join("");
    Ok(format!("{attributes}{}{constraints}", tparam.name.1))
}

fn fmt_fun_hint(tparams: &[&str], function: &HintFun) -> Result<String> {
    let tparams = tparams
        .iter()
        .copied()
        .chain(function.tparams.iter().map(|tparam| tparam.name.1.as_str()))
        .collect::<Vec<_>>();
    let tparams = tparams.as_slice();
    let type_parameters = if function.tparams.is_empty() {
        String::new()
    } else {
        format!(
            "<{}>",
            function
                .tparams
                .iter()
                .map(|tparam| fmt_fun_tparam(tparams, tparam))
                .collect::<Result<Vec<_>>>()?
                .join(", ")
        )
    };
    let variadic = function
        .variadic_ty
        .as_ref()
        .map(|hint| fmt_hint(tparams, false, hint).map(|hint| format!("{hint}...")))
        .transpose()?;
    let named_variadic = function
        .named_variadic_ty
        .as_ref()
        .map(|hint| fmt_hint(tparams, false, hint).map(|hint| format!("named {hint}...")))
        .transpose()?;
    let parameters = function
        .param_tys
        .iter()
        .enumerate()
        .map(|(index, hint)| {
            fmt_fun_param(
                tparams,
                hint,
                function.param_info.get(index).and_then(Option::as_ref),
            )
        })
        .chain(variadic.into_iter().chain(named_variadic).map(Ok))
        .collect::<Result<Vec<_>>>()?
        .join(", ");
    let contexts = function
        .ctxs
        .as_ref()
        .map(|contexts| {
            fmt_hints(tparams, &contexts.1, ", ").map(|contexts| format!("[{contexts}]"))
        })
        .transpose()?
        .unwrap_or_default();
    let readonly = if function.is_readonly.is_some() {
        "readonly "
    } else {
        ""
    };
    let readonly_return = if function.is_readonly_return.is_some() {
        "readonly "
    } else {
        ""
    };
    Ok(format!(
        "({readonly}function{type_parameters} ({parameters}){contexts}: {readonly_return}{})",
        fmt_hint(tparams, false, &function.return_ty)?
    ))
}

fn can_be_nullable(hint: &Hint_) -> bool {
    match hint {
        Haccess(_, _) | Hfun(_) | Hdynamic | Hnonnull | Hmixed | Hwildcard => false,
        Hoption(Hint(_, h)) => {
            if let Haccess(_, _) = **h {
                true
            } else {
                can_be_nullable(h)
            }
        }
        Happly(Id(_, id), _) => {
            id != "\\HH\\dynamic" && id != "\\HH\\nonnull" && id != "\\HH\\mixed"
        }
        Habstr(_)
        | HclassPtr(_, _)
        | HfunContext(_)
        | Hintersection(_)
        | Hlike(_)
        | Hnothing
        | Hprim(_)
        | Hrefinement(_, _)
        | Hshape(_)
        | Hsoft(_)
        | Hthis
        | Htuple(_)
        | Hunion(_)
        | Hvar(_)
        | HvecOrDict(_, _) => true,
    }
}

fn hint_to_type_constraint(
    kind: &Kind,
    tparams: &[&str],
    skipawaitable: bool,
    h: &Hint,
) -> Result<Constraint> {
    let Hint(_, hint) = h;
    Ok(match &**hint {
        Hdynamic | Hfun(_) | Hunion(_) | Hintersection(_) | Hmixed | Hwildcard => {
            Constraint::default()
        }
        Haccess(_, _) => Constraint::intern("", TypeConstraintFlags::TypeConstant),
        Hshape(_) => Constraint::intern("HH\\darray", TypeConstraintFlags::NoFlags),
        Htuple(_) => Constraint::intern("HH\\varray", TypeConstraintFlags::NoFlags),
        Hsoft(t) => make_tc_with_flags_if_non_empty_flags(
            kind,
            tparams,
            skipawaitable,
            t,
            TypeConstraintFlags::Soft,
        )?,
        Hlike(h) => hint_to_type_constraint(kind, tparams, skipawaitable, h)?,
        Hoption(t) => {
            if let Happly(Id(_, s), hs) = &*(t.1) {
                if skipawaitable && is_awaitable(s) {
                    match &hs[..] {
                        [] => return Ok(Constraint::default()),
                        [h] => return hint_to_type_constraint(kind, tparams, false, h),
                        _ => {}
                    }
                }
            } else if let Hsoft(Hint(_, h)) = &*(t.1) {
                if let Happly(Id(_, s), hs) = &**h {
                    if skipawaitable && is_awaitable(s) {
                        if let [h] = &hs[..] {
                            return make_tc_with_flags_if_non_empty_flags(
                                kind,
                                tparams,
                                skipawaitable,
                                h,
                                TypeConstraintFlags::Soft,
                            );
                        }
                    }
                }
            }
            make_tc_with_flags_if_non_empty_flags(
                kind,
                tparams,
                skipawaitable,
                t,
                TypeConstraintFlags::Nullable | TypeConstraintFlags::DisplayNullable,
            )?
        }
        Happly(Id(_, s), hs) => {
            match &hs[..] {
                [] if s == "\\HH\\dynamic"
                    || s == "\\HH\\mixed"
                    || (skipawaitable && is_awaitable(s))
                    || (s.eq_ignore_ascii_case("\\HH\\void") && !is_typedef(kind)) =>
                {
                    return Ok(Constraint::default());
                }
                [Hint(_, h)] if skipawaitable && is_awaitable(s) => {
                    return match &**h {
                        Hprim(Tprim::Tvoid) => Ok(Constraint::default()),
                        Happly(Id(_, id), hs) if id == "\\HH\\void" && hs.is_empty() => {
                            Ok(Constraint::default())
                        }
                        _ => hint_to_type_constraint(kind, tparams, false, &hs[0]),
                    };
                }
                [h] if s == typehints::POISON_MARKER || s == typehints::HH_FUNCTIONREF => {
                    return hint_to_type_constraint(kind, tparams, false, h);
                }
                [h] if is_representable_as(s) => {
                    return hint_to_type_constraint(kind, tparams, false, h);
                }
                _ => {}
            };
            type_application_helper(tparams, kind, s)?
        }
        HclassPtr(_, _) => {
            Constraint::intern(hhbc::BUILTIN_NAME_CLASS, TypeConstraintFlags::NoFlags)
        }
        Habstr(s) => type_application_helper(tparams, kind, s)?,
        Hrefinement(hint, _) => {
            // NOTE: refinements are already banned in type structures
            // and in other cases they should be invisible to the HHVM, so unpack hint
            hint_to_type_constraint(kind, tparams, skipawaitable, hint)?
        }
        // TODO: should probably just return Result::Err for some of these
        HfunContext(_) | Hnonnull | Hnothing | Hprim(_) | Hthis | Hvar(_) | HvecOrDict(_, _) => {
            type_application_helper(tparams, kind, hint_to_string(hint))?
        }
    })
}

fn is_awaitable(s: &str) -> bool {
    s == classes::AWAITABLE
}

fn is_representable_as(s: &str) -> bool {
    s == classes::REPRESENTABLE_AS
}

fn is_typedef(kind: &Kind) -> bool {
    Kind::TypeDef == *kind
}

fn make_tc_with_flags_if_non_empty_flags(
    kind: &Kind,
    tparams: &[&str],
    skipawaitable: bool,
    hint: &Hint,
    flags: TypeConstraintFlags,
) -> Result<Constraint> {
    let tc = hint_to_type_constraint(kind, tparams, skipawaitable, hint)?;
    Ok(match (&tc.name, u16::from(&tc.flags)) {
        (Nothing, 0) => tc,
        _ => Constraint {
            name: tc.name,
            flags: flags | tc.flags,
        },
    })
}

// Used for nodes that do type application (i.e., Happly)
fn type_application_helper(tparams: &[&str], kind: &Kind, name: &str) -> Result<Constraint> {
    if tparams.contains(&name) {
        let tc_name = match kind {
            Kind::Param | Kind::Return | Kind::Property => Just(hhbc::intern(name)),
            _ => Just(hhbc::StringId::EMPTY),
        };
        Ok(Constraint {
            name: tc_name,
            flags: TypeConstraintFlags::TypeVar,
        })
    } else {
        let name = ClassName::mangle(name);
        Ok(Constraint {
            name: Just(name),
            flags: TypeConstraintFlags::NoFlags,
        })
    }
}

fn add_nullable(nullable: bool, flags: TypeConstraintFlags) -> TypeConstraintFlags {
    if nullable {
        TypeConstraintFlags::Nullable | TypeConstraintFlags::DisplayNullable | flags
    } else {
        flags
    }
}

fn try_add_nullable(
    nullable: bool,
    hint: &Hint,
    flags: TypeConstraintFlags,
) -> TypeConstraintFlags {
    let Hint(_, h) = hint;
    add_nullable(nullable && can_be_nullable(h), flags)
}

fn make_type_info(
    tparams: &[&str],
    h: &Hint,
    tc_name: Maybe<StringId>,
    tc_flags: TypeConstraintFlags,
) -> Result<TypeInfo> {
    let type_info_user_type = fmt_hint(tparams, false, h)?;
    let type_info_type_constraint = Constraint::new(tc_name, tc_flags);
    Ok(TypeInfo::new(
        Just(hhbc::intern(type_info_user_type)),
        type_info_type_constraint,
    ))
}

fn param_hint_to_type_info(
    kind: &Kind,
    skipawaitable: bool,
    nullable: bool,
    tparams: &[&str],
    hint: &Hint,
) -> Result<TypeInfo> {
    let Hint(_, h) = hint;
    let is_simple_hint = match h.as_ref() {
        Hlike(_)
        | Hsoft(_)
        | Hoption(_)
        | Haccess(_, _)
        | Hfun(_)
        | Hdynamic
        | Hnonnull
        | Hmixed => false,
        Happly(Id(_, s), hs) => {
            hs.is_empty()
                && s != "\\HH\\dynamic"
                && s != "\\HH\\nonnull"
                && s != "\\HH\\mixed"
                && !tparams.contains(&s.as_str())
        }
        Habstr(s) => !tparams.contains(&s.as_str()),
        Hprim(_)
        | Htuple(_)
        | HclassPtr(_, _)
        | Hshape(_)
        | Hrefinement(_, _)
        | Hwildcard
        | HvecOrDict(_, _)
        | Hthis
        | Hnothing
        | Hunion(_)
        | Hintersection(_)
        | HfunContext(_)
        | Hvar(_) => true,
    };
    let tc = hint_to_type_constraint(kind, tparams, skipawaitable, hint)?;
    make_type_info(
        tparams,
        hint,
        tc.name,
        try_add_nullable(
            nullable,
            hint,
            if is_simple_hint {
                TypeConstraintFlags::NoFlags
            } else {
                tc.flags
            },
        ),
    )
}

pub fn hint_to_type_info(
    kind: &Kind,
    skipawaitable: bool,
    nullable: bool,
    tparams: &[&str],
    hint: &Hint,
) -> Result<TypeInfo> {
    if let Kind::Param = kind {
        return param_hint_to_type_info(kind, skipawaitable, nullable, tparams, hint);
    };
    let tc = hint_to_type_constraint(kind, tparams, skipawaitable, hint)?;
    let flags = match kind {
        Kind::UpperBound => TypeConstraintFlags::UpperBound | tc.flags,
        _ => tc.flags,
    };
    make_type_info(
        tparams,
        hint,
        tc.name,
        if is_typedef(kind) {
            add_nullable(nullable, flags)
        } else {
            try_add_nullable(nullable, hint, flags)
        },
    )
}

// Used from emit_typedef for potential case types
pub fn hint_to_type_info_union(
    kind: &Kind,
    skipawaitable: bool,
    nullable: bool,
    tparams: &[&str],
    hint: &Hint,
) -> Result<Vec<TypeInfo>> {
    let Hint(_, h) = hint;
    let mut result = vec![];
    match &**h {
        Hunion(hints) => {
            for hint in hints {
                result.push(hint_to_type_info(
                    kind,
                    skipawaitable,
                    nullable,
                    tparams,
                    hint,
                )?)
            }
        }
        _ => result.push(hint_to_type_info(
            kind,
            skipawaitable,
            nullable,
            tparams,
            hint,
        )?),
    }
    Ok(result)
}

pub fn hint_to_class(hint: &Hint) -> ClassName {
    let Hint(_, h) = hint;
    if let Happly(Id(_, name), _) = &**h {
        ClassName::from_ast_name_and_mangle(name)
    } else {
        ClassName::new(string_id!("__type_is_not_class__"))
    }
}

// TODO(T206014630): This function and get_flags should likely be deleted now that
// <<__Native>> functions do regular type enforcement
pub fn emit_type_constraint_for_native_function(
    tparams: &[&str],
    ret_opt: Option<&Hint>,
    ti: TypeInfo,
) -> TypeInfo {
    let (name, flags) = match (&ti.user_type, ret_opt) {
        (_, None) | (Nothing, _) => (Just("HH\\void"), TypeConstraintFlags::NoFlags),
        (Just(t), _) => match t.as_str() {
            "HH\\mixed" | "callable" => (Nothing, TypeConstraintFlags::default()),
            "HH\\void" => {
                let Hint(_, h) = ret_opt.as_ref().unwrap();
                (
                    Just("HH\\void"),
                    get_flags(tparams, TypeConstraintFlags::NoFlags, h),
                )
            }
            _ => return ti,
        },
    };
    let tc = Constraint::new(name.map(hhbc::intern), flags);
    TypeInfo::new(ti.user_type, tc)
}

fn get_flags(tparams: &[&str], flags: TypeConstraintFlags, hint: &Hint_) -> TypeConstraintFlags {
    match hint {
        Hoption(x) => {
            let Hint(_, h) = x;
            TypeConstraintFlags::Nullable
                | TypeConstraintFlags::DisplayNullable
                | get_flags(tparams, flags, h)
        }
        Hsoft(x) => {
            let Hint(_, h) = x;
            TypeConstraintFlags::Soft | get_flags(tparams, flags, h)
        }
        Haccess(_, _) => TypeConstraintFlags::TypeConstant | flags,
        Happly(Id(_, s), _) if tparams.contains(&s.as_str()) => {
            TypeConstraintFlags::TypeVar | flags
        }
        Habstr(_)
        | Happly(_, _)
        | HclassPtr(_, _)
        | Hdynamic
        | Hfun(_)
        | HfunContext(_)
        | Hintersection(_)
        | Hlike(_)
        | Hmixed
        | Hnonnull
        | Hnothing
        | Hprim(_)
        | Hrefinement(_, _)
        | Hshape(_)
        | Hthis
        | Htuple(_)
        | Hunion(_)
        | Hvar(_)
        | HvecOrDict(_, _)
        | Hwildcard => flags,
    }
}

#[cfg(test)]
mod tests {
    use hhbc::Constraint;
    use hhvm_types_ffi::ffi::TypeConstraintFlags;
    use oxidized::aast_defs::Contexts;
    use oxidized::aast_defs::HfParamInfo;
    use oxidized::aast_defs::Hint;
    use oxidized::aast_defs::Hint_;
    use oxidized::aast_defs::HintFun;
    use oxidized::aast_defs::HintTparam;
    use oxidized::aast_defs::TupleExtra;
    use oxidized::aast_defs::TupleExtraInfo;
    use oxidized::aast_defs::TupleInfo;
    use oxidized::ast_defs::ConstraintKind;
    use oxidized::ast_defs::Id;
    use oxidized::ast_defs::OptionalKind;
    use oxidized::ast_defs::ParamKind;
    use oxidized::ast_defs::ReadonlyKind;
    use oxidized::ast_defs::SplatKind;
    use oxidized::pos::Pos;

    use super::Kind;
    use super::fmt_hint;
    use super::hint_to_type_info;

    fn hint(kind: Hint_) -> Hint {
        Hint(Pos::NONE, Box::new(kind))
    }

    fn apply(name: &str) -> Hint {
        hint(Hint_::Happly(Id(Pos::NONE, name.into()), vec![]))
    }

    fn param_info() -> HfParamInfo {
        HfParamInfo {
            kind: ParamKind::Pnormal,
            readonlyness: None,
            optional: None,
            splat: None,
            named: None,
        }
    }

    fn signature(param_tys: Vec<Hint>, return_ty: Hint) -> HintFun {
        let param_info = param_tys.iter().map(|_| None).collect();
        HintFun {
            is_readonly: None,
            tparams: vec![],
            param_tys,
            param_info,
            variadic_ty: None,
            named_variadic_ty: None,
            ctxs: None,
            return_ty,
            is_readonly_return: None,
        }
    }

    fn rich_function() -> Hint {
        let mut function = signature(
            vec![apply("Widget"), apply("\\HH\\int"), apply("\\HH\\bool")],
            apply("Widget"),
        );
        function.param_info = vec![
            Some(HfParamInfo {
                readonlyness: Some(ReadonlyKind::Readonly),
                ..param_info()
            }),
            Some(HfParamInfo {
                kind: ParamKind::Pinout(Pos::NONE),
                ..param_info()
            }),
            Some(HfParamInfo {
                optional: Some(OptionalKind::Optional),
                ..param_info()
            }),
        ];
        function.is_readonly = Some(ReadonlyKind::Readonly);
        function.is_readonly_return = Some(ReadonlyKind::Readonly);
        function.variadic_ty = Some(apply("\\HH\\string"));
        function.named_variadic_ty = Some(apply("\\HH\\num"));
        function.ctxs = Some(Contexts(Pos::NONE, vec![apply("io"), apply("write_props")]));
        hint(Hint_::Hfun(function))
    }

    fn tuple(required: Vec<Hint>, optional: Vec<Hint>, variadic: Option<Hint>) -> Hint {
        hint(Hint_::Htuple(TupleInfo {
            required,
            extra: TupleExtra::Hextra(TupleExtraInfo { optional, variadic }),
        }))
    }

    #[test]
    fn function_preserves_modifiers_variadics_and_contexts() {
        assert_eq!(
            fmt_hint(&[], false, &rich_function()).expect("function should format"),
            "(readonly function (readonly Widget, inout HH\\int, optional HH\\bool, HH\\string..., named HH\\num...)[io, write_props]: readonly Widget)",
        );
    }

    #[test]
    fn function_distinguishes_default_contexts_from_explicit_pure() {
        let mut function = signature(vec![], apply("\\HH\\void"));
        assert_eq!(
            fmt_hint(&[], false, &hint(Hint_::Hfun(function.clone())))
                .expect("default contexts should format"),
            "(function (): HH\\void)",
        );
        function.ctxs = Some(Contexts(Pos::NONE, vec![]));
        assert_eq!(
            fmt_hint(&[], false, &hint(Hint_::Hfun(function)))
                .expect("pure contexts should format"),
            "(function ()[]: HH\\void)",
        );
    }

    #[test]
    fn function_preserves_generic_bounds_attributes_named_parameters_and_splats() {
        let mut function = signature(vec![apply("T"), apply("Args")], apply("T"));
        function.tparams = vec![HintTparam {
            name: Id(Pos::NONE, "T".into()),
            user_attributes: vec![Id(Pos::NONE, "__NoAutoBound".into())],
            constraints: vec![
                (ConstraintKind::ConstraintAs, apply("Upper")),
                (ConstraintKind::ConstraintSuper, apply("Lower")),
            ],
        }];
        function.param_info = vec![
            Some(HfParamInfo {
                named: Some("$value".into()),
                ..param_info()
            }),
            Some(HfParamInfo {
                splat: Some(SplatKind::Splat),
                ..param_info()
            }),
        ];
        assert_eq!(
            fmt_hint(&[], false, &hint(Hint_::Hfun(function)))
                .expect("generic function should format"),
            "(function<<<__NoAutoBound>> T as Upper super Lower> (named T $value, ...Args): T)",
        );
    }

    #[test]
    fn tuple_preserves_optional_variadic_and_splat_elements() {
        let cases = [
            (
                tuple(
                    vec![apply("\\HH\\int")],
                    vec![apply("\\HH\\string")],
                    Some(apply("\\HH\\bool")),
                ),
                "(HH\\int, optional HH\\string, HH\\bool...)",
            ),
            (
                tuple(vec![], vec![apply("\\HH\\int")], None),
                "(optional HH\\int)",
            ),
            (
                tuple(vec![], vec![], Some(apply("\\HH\\string"))),
                "(HH\\string...)",
            ),
            (
                hint(Hint_::Htuple(TupleInfo {
                    required: vec![apply("\\HH\\int")],
                    extra: TupleExtra::Hsplat(apply("Args")),
                })),
                "(HH\\int, ...Args)",
            ),
        ];
        for (tuple, expected) in cases {
            assert_eq!(
                fmt_hint(&[], false, &tuple).expect("tuple should format"),
                expected
            );
        }
    }

    #[test]
    fn auxiliary_contexts_and_vec_or_dict_preserve_their_arguments() {
        assert_eq!(
            fmt_hint(&[], false, &hint(Hint_::HfunContext("$cb".into())))
                .expect("function context should format"),
            "ctx $cb",
        );
        let access = hint(Hint_::Haccess(
            hint(Hint_::Hvar("$obj".into())),
            vec![Id(Pos::NONE, "C".into()), Id(Pos::NONE, "D".into())],
        ));
        assert_eq!(
            fmt_hint(&[], false, &access).expect("variable context should format"),
            "$obj::C::D"
        );
        for (key, expected) in [
            (None, "HH\\vec_or_dict<HH\\int>"),
            (
                Some(apply("\\HH\\arraykey")),
                "HH\\vec_or_dict<HH\\arraykey, HH\\int>",
            ),
        ] {
            let array = hint(Hint_::HvecOrDict(key, apply("\\HH\\int")));
            assert_eq!(
                fmt_hint(&[], false, &array).expect("array arguments should format"),
                expected
            );
        }
    }

    #[test]
    fn reflection_detail_does_not_change_enforced_constraints() {
        let function = rich_function();
        let tuple = tuple(
            vec![apply("\\HH\\int")],
            vec![apply("\\HH\\string")],
            Some(apply("\\HH\\bool")),
        );
        for kind in [Kind::Param, Kind::Return, Kind::Property] {
            for (hint, expected) in [
                (&function, Constraint::default()),
                (
                    &tuple,
                    Constraint::intern("HH\\varray", TypeConstraintFlags::NoFlags),
                ),
            ] {
                let info = hint_to_type_info(&kind, false, false, &[], hint)
                    .expect("constraint should emit");
                assert_eq!(info.type_constraint, expected);
            }
        }
    }

    #[test]
    fn enum_generic_erasure_remains_explicit() {
        let generic = hint(Hint_::Happly(
            Id(Pos::NONE, "Box".into()),
            vec![apply("\\HH\\int")],
        ));
        assert_eq!(
            fmt_hint(&[], false, &generic).expect("generic should format"),
            "Box<HH\\int>"
        );
        assert_eq!(
            fmt_hint(&[], true, &generic).expect("enum base should erase arguments"),
            "Box"
        );
    }
}
