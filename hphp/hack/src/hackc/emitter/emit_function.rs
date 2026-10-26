// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::sync::Arc;

use ast_scope::Scope;
use ast_scope::ScopeItem;
use env::emitter::Emitter;
use error::Result;
use hhbc::Attr;
use hhbc::ClassName;
use hhbc::Coeffects;
use hhbc::Function;
use hhbc::FunctionName;
use hhbc::Span;
use instruction_sequence::instr;
use naming_special_names_rust::user_attributes as ua;
use oxidized::ast;
use oxidized::ast_defs;

use crate::emit_attribute;
use crate::emit_body;
use crate::emit_memoize_function;
use crate::emit_memoize_helpers;
use crate::emit_param;

pub fn emit_function<'a>(e: &mut Emitter, fd: &'a ast::FunDef) -> Result<Vec<Function>> {
    use ast_defs::FunKind;
    use hhbc::FunctionFlags;

    let f = &fd.fun;
    let original_id = FunctionName::from_ast_name(&fd.name.1);
    let mut flags = FunctionFlags::empty();
    flags.set(
        FunctionFlags::ASYNC,
        matches!(f.fun_kind, FunKind::FAsync | FunKind::FAsyncGenerator),
    );
    let mut user_attrs = emit_attribute::from_asts(e, &f.user_attributes)?;
    user_attrs.extend(emit_attribute::add_reified_attribute(&fd.tparams));
    let memoized = user_attrs.iter().any(|a| ua::is_memoized(a.name.as_str()));
    flags.set(FunctionFlags::MEMOIZE_IMPL, memoized);

    let renamed_id = {
        if memoized {
            FunctionName::add_suffix(&original_id, emit_memoize_helpers::MEMOIZE_SUFFIX)
        } else {
            original_id
        }
    };
    let is_meth_caller = fd.name.1.starts_with("\\MethCaller$");
    let call_context = if is_meth_caller {
        match &f.user_attributes[..] {
            [
                ast::UserAttribute {
                    name: ast_defs::Id(_, s),
                    params,
                },
            ] if s == "__MethCaller" => match &params[..] {
                [ast::Expr(_, _, ast::Expr_::String(ctx))] if !ctx.is_empty() => {
                    match std::str::from_utf8(ctx) {
                        Ok(ctx) => Some(ClassName::mangle(ctx.to_owned())),
                        Err(_) => {
                            // non-utf8 classname cant be a valid context
                            None
                        }
                    }
                }
                _ => None,
            },
            _ => None,
        }
    } else {
        None
    };
    let is_debug_main = match f.user_attributes.as_slice() {
        [ast::UserAttribute { name, params }]
            if name.1 == "__DebuggerMain" && params.is_empty() =>
        {
            true
        }
        _ => false,
    };
    let mut scope = Scope::default();
    if !is_debug_main {
        scope.push_item(ScopeItem::Function(ast_scope::Fun::new_ref(fd)));
    }

    let mut coeffects = Coeffects::from_ast(f.ctxs.as_ref(), &f.params, &fd.tparams, vec![]);
    if is_meth_caller {
        coeffects = coeffects.with_caller()
    }
    if e.systemlib()
        && (fd.name.1 == "\\HH\\Coeffects\\backdoor"
            || fd.name.1 == "\\HH\\Coeffects\\backdoor_async")
    {
        coeffects = coeffects.with_backdoor()
    }
    if e.systemlib()
        && (fd.name.1 == "\\HH\\Coeffects\\fb\\backdoor_to_globals_leak_safe__DO_NOT_USE"
            || fd.name.1 == "\\HH\\Coeffects\\fb\\backdoor_to_globals_leak_safe_async__DO_NOT_USE")
    {
        coeffects = coeffects.with_backdoor_globals_leak_safe()
    }
    let memoize_wrapper = if memoized {
        let deprecation_info = hhbc::deprecation_info(&user_attrs);
        Some(emit_memoize_function::emit_wrapper_function(
            e,
            original_id,
            renamed_id,
            deprecation_info,
            fd,
        )?)
    } else {
        None
    };
    let (mut body, is_gen, is_pair_gen) = {
        let native = user_attrs.iter().any(|a| ua::is_native(a.name.as_str()));
        use emit_body::Args as EmitBodyArgs;
        use emit_body::Flags as EmitBodyFlags;
        let mut body_flags = EmitBodyFlags::empty();
        body_flags.set(EmitBodyFlags::ASYNC, flags.contains(FunctionFlags::ASYNC));
        body_flags.set(EmitBodyFlags::NATIVE, native);
        body_flags.set(EmitBodyFlags::MEMOIZE, memoized);
        body_flags.set(
            EmitBodyFlags::SKIP_AWAITABLE,
            f.fun_kind == ast_defs::FunKind::FAsync,
        );
        body_flags.set(
            EmitBodyFlags::HAS_COEFFECTS_LOCAL,
            coeffects.has_coeffects_local(),
        );

        emit_body::emit_body(
            e,
            Arc::clone(&fd.namespace),
            &f.body.fb_ast,
            instr::null(),
            scope,
            Span::from_pos(&f.span),
            user_attrs,
            Attr::AttrNone,
            coeffects,
            EmitBodyArgs {
                flags: body_flags,
                emit_deprecation_info: !memoized,
                default_dropthrough: None,
                doc_comment: f.doc_comment.clone(),
                pos: &f.span,
                ret: f.ret.1.as_ref(),
                ast_params: &f.params,
                call_context,
                immediate_tparams: &fd.tparams,
                class_tparam_names: &[],
            },
        )?
    };
    flags.set(FunctionFlags::GENERATOR, is_gen);
    flags.set(FunctionFlags::PAIR_GENERATOR, is_pair_gen);
    let has_variadic = emit_param::has_variadic(&body.repr.params);
    let has_variadic_named = emit_param::has_variadic_named(&f.params);
    let has_splat = emit_param::has_splat(&body.repr.params);
    body.attrs = emit_memoize_function::get_attrs_for_fun(
        e,
        fd,
        &body.attributes,
        memoized,
        has_variadic,
        has_variadic_named,
        has_splat,
    );
    let normal_function = Function {
        name: renamed_id,
        body,
        flags,
    };

    Ok(if let Some(memoize_wrapper) = memoize_wrapper {
        vec![normal_function, memoize_wrapper]
    } else {
        vec![normal_function]
    })
}

pub fn emit_functions_from_program<'a>(
    e: &mut Emitter,
    ast: &'a [ast::Def],
) -> Result<Vec<Function>> {
    Ok(ast
        .iter()
        .filter_map(|d| d.as_fun().map(|f| emit_function(e, f)))
        .collect::<Result<Vec<Vec<_>>>>()?
        .into_iter()
        .flatten()
        .collect::<Vec<_>>())
}
