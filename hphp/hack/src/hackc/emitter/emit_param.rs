// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::marker::PhantomData;

use ast_scope::Scope;
use emit_type_hint::Kind;
use emit_type_hint::hint_to_type_info;
use env::Env;
use env::emitter::Emitter;
use error::Error;
use error::Result;
use ffi::Maybe;
use hhbc::Label;
use hhbc::Local;
use hhbc::Param;
use hhbc::ParamEntry;
use hhbc::StringIdMap;
use hhbc::StringIdSet;
use hhbc::TypeInfo;
use hhbc_string_utils::locals::strip_dollar;
use instruction_sequence::InstrSeq;
use instruction_sequence::instr;
use oxidized::aast_defs::Hint;
use oxidized::aast_defs::Hint_;
use oxidized::aast_visitor;
use oxidized::aast_visitor::AstParams;
use oxidized::aast_visitor::Node;
use oxidized::ast as a;
use oxidized::ast::ParamNamed;
use oxidized::ast_defs::Id;
use oxidized::ast_defs::ReadonlyKind;
use oxidized::pos::Pos;

use crate::emit_attribute;
use crate::emit_expression;

pub fn has_variadic(params: &[ParamEntry]) -> bool {
    params
        .iter()
        .rev()
        .any(|p| p.param.is_variadic || p.param.is_splat)
}

pub fn has_splat(params: &[ParamEntry]) -> bool {
    params.iter().rev().any(|p| p.param.is_splat)
}

/// Whether the function declares a `named t...` parameter. Such a parameter has
/// no name, so `from_ast` drops it rather than emitting a parameter nothing can
/// bind; callers record it as `AttrVariadicNamedParam` instead. Note this takes
/// the AST parameters, not the emitted ones, for that reason.
pub fn has_variadic_named(ast_params: &[a::FunParam]) -> bool {
    ast_params.iter().any(|p| {
        matches!(p.info, a::FunParamInfo::ParamVariadic) && p.named.is_some() && p.name == "..."
    })
}

pub fn from_asts<'a>(
    emitter: &mut Emitter,
    tparams: &mut Vec<&str>,
    generate_defaults: bool,
    scope: &Scope<'a>,
    ast_params: &[a::FunParam],
) -> Result<Vec<(Param, Option<(Label, a::Expr)>)>> {
    ast_params
        .iter()
        .map(|param| from_ast(emitter, tparams, generate_defaults, scope, param))
        .collect::<Result<Vec<_>>>()
        .map(|params| {
            params
                .iter()
                .filter_map(|p| p.to_owned())
                .collect::<Vec<_>>()
        })
        .map(rename_params)
}

fn rename_params(
    mut params: Vec<(Param, Option<(Label, a::Expr)>)>,
) -> Vec<(Param, Option<(Label, a::Expr)>)> {
    let mut param_counts = StringIdMap::default();
    let names: StringIdSet = params.iter().map(|(p, _)| p.name).collect();
    for (param, _) in params.iter_mut() {
        use std::collections::hash_map::Entry;
        'inner: loop {
            match param_counts.entry(param.name) {
                Entry::Vacant(e) => {
                    e.insert(0);
                }
                Entry::Occupied(mut e) => {
                    let newname = format!("{}{}", param.name, e.get());
                    let newname = hhbc::intern(newname);
                    *e.get_mut() += 1;
                    if names.contains(&newname) {
                        // collision - try again
                        continue 'inner;
                    } else {
                        param.name = newname;
                    }
                }
            }
            break 'inner;
        }
    }
    params.into_iter().collect()
}

fn from_ast<'a>(
    emitter: &mut Emitter,
    tparams: &mut Vec<&str>,
    generate_defaults: bool,
    scope: &Scope<'a>,
    param: &a::FunParam,
) -> Result<Option<(Param, Option<(Label, a::Expr)>)>> {
    let is_variadic = match param.info {
        a::FunParamInfo::ParamVariadic => true,
        a::FunParamInfo::ParamRequired => false,
        a::FunParamInfo::ParamOptional(_) => false,
    };
    let is_splat = param.splat.is_some();
    if is_variadic && param.name == "..." {
        return Ok(None);
    }
    // Variadic and splat parameters are treated the same by HackC and HHVM
    // The former is a vec (represented by an array) all of whose elements are the same type
    // The latter is a tuple (represented by an array) whose elements many vary in type
    if is_variadic || is_splat {
        tparams.push("array");
    }
    let type_info = {
        let param_type_hint = match param.info {
            a::FunParamInfo::ParamVariadic => Some(Hint(
                Pos::NONE,
                Box::new(Hint_::mk_happly(
                    Id(Pos::NONE, "array".to_string()),
                    param
                        .type_hint
                        .get_hint()
                        .as_ref()
                        .map_or(vec![], |h| vec![h.clone()]),
                )),
            )),
            a::FunParamInfo::ParamRequired | a::FunParamInfo::ParamOptional(_) => {
                param.type_hint.get_hint().clone()
            }
        };
        if let Some(h) = param_type_hint {
            Some(hint_to_type_info(
                &Kind::Param,
                false,
                false, /* meaning only set nullable based on given hint */
                &tparams[..],
                &h,
            )?)
        } else {
            None
        }
    };
    let expr = match &param.info {
        a::FunParamInfo::ParamOptional(Some(expr)) => Some(expr),
        a::FunParamInfo::ParamOptional(None)
        | a::FunParamInfo::ParamRequired
        | a::FunParamInfo::ParamVariadic => None,
    };
    // Do the type check for default value type and hint type
    if let Some(err_msg) = default_type_check(&param.name, type_info.as_ref(), expr) {
        return Err(Error::fatal_parse(&param.pos, err_msg));
    }
    aast_visitor::visit(
        &mut ResolverVisitor {
            phantom_a: PhantomData,
            phantom_d: PhantomData,
        },
        &mut Ctx { emitter, scope },
        &expr,
    )
    .unwrap();
    let default_value = if generate_defaults {
        expr.map(|expr| (emitter.label_gen_mut().next_regular(), expr.clone()))
    } else {
        None
    };
    let is_readonly = match param.readonly {
        Some(ReadonlyKind::Readonly) => true,
        _ => false,
    };
    let is_named = match param.named {
        Some(ParamNamed::ParamNamed) => true,
        _ => false,
    };
    let is_optional = match &param.info {
        a::FunParamInfo::ParamOptional(None) => true,
        a::FunParamInfo::ParamOptional(Some(_))
        | a::FunParamInfo::ParamRequired
        | a::FunParamInfo::ParamVariadic => false,
    };
    let attrs = emit_attribute::from_asts(emitter, &param.user_attributes)?;
    Ok(Some((
        Param {
            name: hhbc::intern(&param.name),
            is_variadic,
            is_splat,
            is_inout: param.callconv.is_pinout(),
            is_readonly,
            is_optional,
            is_named,
            user_attributes: attrs.into(),
            type_info: Maybe::from(type_info),
        },
        default_value,
    )))
}

pub fn emit_param_default_value_setter<'a>(
    emitter: &mut Emitter,
    env: &Env<'a>,
    params: &[(Param, Option<(Label, a::Expr)>)],
    ast_params: &[a::FunParam],
) -> Result<(InstrSeq, InstrSeq)> {
    let setters = params
        .iter()
        .zip(ast_params.into_iter())
        .enumerate()
        .filter_map(|(i, ((param, dv), ast_param))| {
            // LocalIds for params are numbered from 0.
            dv.as_ref().map(|(lbl, expr)| {
                let param_local = Local::new(i);
                let instrs = if param.is_named {
                    let skip = emitter.label_gen_mut().next_regular();
                    InstrSeq::gather(vec![
                        instr::is_unset_l(param_local),
                        instr::jmp_z(skip),
                        emit_expression::emit_expr(emitter, env, expr)?,
                        emit_pos::emit_pos(&ast_param.pos),
                        instr::verify_param_type(param_local),
                        instr::pop_l(param_local),
                        instr::label(skip),
                    ])
                } else {
                    InstrSeq::gather(vec![
                        emit_expression::emit_expr(emitter, env, expr)?,
                        emit_pos::emit_pos(&ast_param.pos),
                        instr::verify_param_type(param_local),
                        instr::set_l(param_local),
                        instr::pop_c(),
                    ])
                };
                Ok((
                    i,
                    InstrSeq::gather(vec![instr::label(lbl.to_owned()), instrs]),
                ))
            })
        })
        .collect::<Result<Vec<_>>>()?;
    if setters.is_empty() {
        Ok((instr::empty(), instr::empty()))
    } else {
        // We're guaranteed that all named params precede positionals.
        let mut first_positional_idx = 0;
        while first_positional_idx < params.len() {
            if !params[first_positional_idx].0.is_named {
                break;
            }
            first_positional_idx += 1;
        }
        let partition_pos = setters.partition_point(|(idx, _)| *idx < first_positional_idx);
        let mut iter = setters.into_iter().map(|(_, instrs)| instrs);
        let named_setters = iter.by_ref().take(partition_pos).collect();
        let pos_setters: Vec<_> = iter.collect();
        let l = emitter.label_gen_mut().next_regular();
        Ok((
            instr::label(l),
            InstrSeq::gather(vec![
                InstrSeq::gather(pos_setters),
                InstrSeq::gather(named_setters),
                instr::enter(l),
            ]),
        ))
    }
}

struct ResolverVisitor<'a, 'd: 'a> {
    phantom_a: PhantomData<&'a ()>,
    phantom_d: PhantomData<&'d ()>,
}

#[allow(dead_code)]
struct Ctx<'a> {
    emitter: &'a mut Emitter,
    scope: &'a Scope<'a>,
}

impl<'ast, 'a, 'd> aast_visitor::Visitor<'ast> for ResolverVisitor<'a, 'd> {
    type Params = AstParams<Ctx<'a>, ()>;

    fn object(&mut self) -> &mut dyn aast_visitor::Visitor<'ast, Params = Self::Params> {
        self
    }

    fn visit_expr(&mut self, c: &mut Ctx<'a>, p: &a::Expr) -> Result<(), ()> {
        p.recurse(c, self.object())
        // TODO(hrust) implement on_CIexpr & remove dead_code on struct Ctx
    }
}

// Return None if it passes type check, otherwise return error msg
fn default_type_check(
    param_name: &str,
    param_type_info: Option<&TypeInfo>,
    param_expr: Option<&a::Expr>,
) -> Option<String> {
    let hint_type = get_hint_display_name(
        param_type_info
            .as_ref()
            .and_then(|ti| ti.user_type.as_ref().map(|s| s.as_str()).into()),
    );
    // If matches, return None, otherwise return default_type
    let default_type = hint_type.and_then(|ht| match_default_and_hint(ht, param_expr));
    let param_true_name = strip_dollar(param_name);
    default_type.and_then(|dt| hint_type.map(|ht| match ht {
        "class" => format!(
            "Default value for parameter {} with a class type hint can only be NULL",
            param_true_name),
        _ => format!(
            "Default value for parameter {} with type {} needs to have the same type as the type hint {}",
            param_true_name,
            dt,
            ht)
    }))
}

fn get_hint_display_name(hint: Option<&str>) -> Option<&'static str> {
    hint.map(|h| match h {
        "HH\\bool" => "bool",
        "HH\\varray" => "HH\\varray",
        "HH\\darray" => "HH\\darray",
        "HH\\varray_or_darray" => "HH\\varray_or_darray",
        "HH\\vec_or_dict" => "HH\\vec_or_dict",
        "HH\\AnyArray" => "HH\\AnyArray",
        "HH\\int" => "int",
        "HH\\num" => "num",
        "HH\\arraykey" => "arraykey",
        "HH\\float" => "float",
        "HH\\string" => "string",
        _ => "class",
    })
}

// By now only check default type for bool, array, int, float and string.
//    Return None when hint_type and default_value matches (in hh mode,
//    "class" type matches anything). If not, return default_value type string
//    for printing fatal parse error
fn match_default_and_hint(hint_type: &str, param_expr: Option<&a::Expr>) -> Option<&'static str> {
    if hint_type == "class" {
        return None;
    }
    match &param_expr.as_ref() {
        None => None,
        Some(e) => match e.2 {
            a::Expr_::True | a::Expr_::False => match hint_type {
                "bool" => None,
                _ => Some("Boolean"),
            },
            a::Expr_::Int(_) => match hint_type {
                "int" | "num" | "arraykey" | "float" => None,
                _ => Some("Int64"),
            },
            a::Expr_::Float(_) => match hint_type {
                "float" | "num" => None,
                _ => Some("Double"),
            },
            a::Expr_::String(_) => match hint_type {
                "string" | "arraykey" => None,
                _ => Some("String"),
            },
            _ => None,
        },
    }
}
