// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::sync::Arc;

use ast_scope::Scope;
use ast_scope::ScopeItem;
use emit_pos::emit_pos_then;
use env::Env;
use env::emitter::Emitter;
use error::Result;
use hhbc::Attribute;
use hhbc::Body;
use hhbc::ClassName;
use hhbc::Coeffects;
use hhbc::FCallArgs;
use hhbc::FCallArgsFlags;
use hhbc::Function;
use hhbc::FunctionFlags;
use hhbc::Label;
use hhbc::Local;
use hhbc::LocalRange;
use hhbc::Param;
use hhbc::Span;
use hhbc::StringId;
use hhbc::TParamInfo;
use hhbc::TypeInfo;
use hhbc::TypedValue;
use hhbc::VerifyRetKind;
use hhbc_string_utils::reified;
use hhvm_types_ffi::ffi::Attr;
use instruction_sequence::InstrSeq;
use instruction_sequence::instr;
use oxidized::ast;
use oxidized::pos::Pos;

use crate::emit_attribute;
use crate::emit_body;
use crate::emit_memoize_helpers;
use crate::emit_param;

pub(crate) fn get_attrs_for_fun(
    emitter: &mut Emitter,
    fd: &ast::FunDef,
    user_attrs: &[Attribute],
    is_memoize_impl: bool,
    has_variadic: bool,
    has_variadic_named: bool,
    has_splat: bool,
) -> Attr {
    let f = &fd.fun;
    let is_systemlib = emitter.systemlib();
    let is_dyn_call =
        is_systemlib || (hhbc::has_dynamically_callable(user_attrs) && !is_memoize_impl);
    let is_prov_skip_frame = hhbc::has_provenance_skip_frame(user_attrs);
    let is_meth_caller = hhbc::has_meth_caller(user_attrs);

    let mut attrs = Attr::AttrNone;
    attrs.set(Attr::AttrBuiltin, is_meth_caller | is_systemlib);
    attrs.set(Attr::AttrDynamicallyCallable, is_dyn_call);
    attrs.set(Attr::AttrIsFoldable, hhbc::has_foldable(user_attrs));
    attrs.set(Attr::AttrIsMethCaller, is_meth_caller);
    attrs.set(Attr::AttrNoInjection, hhbc::is_no_injection(user_attrs));
    attrs.set(Attr::AttrProvenanceSkipFrame, is_prov_skip_frame);
    attrs.set(Attr::AttrReadonlyReturn, f.readonly_ret.is_some());
    attrs.set(Attr::AttrInternal, fd.internal);
    attrs.set(Attr::AttrVariadicParam, has_variadic);
    attrs.set(Attr::AttrVariadicNamedParam, has_variadic_named);
    attrs.set(Attr::AttrSplatParam, has_splat);
    attrs
}

pub(crate) fn emit_wrapper_function<'a>(
    emitter: &mut Emitter,
    original_id: hhbc::FunctionName,
    renamed_id: hhbc::FunctionName,
    deprecation_info: Option<&[TypedValue]>,
    fd: &'a ast::FunDef,
) -> Result<Function> {
    let f = &fd.fun;
    emit_memoize_helpers::check_memoize_possible(&(fd.name).0, &f.params, false)?;
    let scope = Scope::with_item(ScopeItem::Function(ast_scope::Fun::new_ref(fd)));
    let mut tparams = scope
        .get_tparams()
        .iter()
        .map(|tp| tp.name.1.as_str())
        .collect::<Vec<_>>();
    let tparam_info = fd
        .tparams
        .iter()
        .map(|s| TParamInfo {
            name: ClassName::intern(s.name.1.as_str()),
            shadows_class_tparam: false,
        })
        .collect::<Vec<_>>();
    // Match the impl's param layout, which emit_body reorders.
    let ast_params = emit_body::reorder_params(&f.params);
    let params = emit_param::from_asts(emitter, &mut tparams, true, &scope, &ast_params)?;
    let mut attributes = emit_attribute::from_asts(emitter, &f.user_attributes)?;
    attributes.extend(emit_attribute::add_reified_attribute(&fd.tparams));
    let return_type = emit_body::emit_return_type(
        &tparams,
        f.fun_kind.is_fasync(), /* skip_awaitable */
        f.ret.1.as_ref(),
    )?;
    let is_reified = fd
        .tparams
        .iter()
        .any(|tp| tp.reified.is_reified() || tp.reified.is_soft_reified());
    let coeffects = Coeffects::from_ast(f.ctxs.as_ref(), &f.params, &fd.tparams, vec![]);
    let should_emit_implicit_context = hhbc::is_keyed_by_ic_memoize(attributes.iter());
    // #NotKeyedByICAndLeakIC__DO_NOT_USE won't shard by IC but will access IC
    let is_not_keyed_by_ic_and_leak_ic = hhbc::is_not_keyed_by_ic_and_leak_ic(attributes.iter());

    // This fn either has IC unoptimizable static coeffects, or has any dynamic coeffects
    let has_ic_unoptimizable_coeffects: bool = coeffects.has_ic_unoptimizable_coeffects();
    let should_make_ic_inaccessible: bool = !is_not_keyed_by_ic_and_leak_ic
        && !should_emit_implicit_context
        && has_ic_unoptimizable_coeffects;

    let mut env = Env::default(Arc::clone(&fd.namespace)).with_scope(scope);
    let (instrs, decl_vars) = make_memoize_function_code(
        emitter,
        &mut env,
        &f.span,
        deprecation_info,
        &params,
        &ast_params,
        renamed_id,
        f.fun_kind.is_fasync(),
        is_reified,
        should_emit_implicit_context,
        should_make_ic_inaccessible,
    )?;
    let mut body = make_wrapper_body(
        emitter,
        env,
        return_type,
        attributes,
        coeffects,
        params,
        decl_vars,
        instrs,
        Span::from_pos(&f.span),
        tparam_info,
    )?;

    let mut flags = FunctionFlags::empty();
    flags.set(FunctionFlags::ASYNC, f.fun_kind.is_fasync());
    let has_variadic = emit_param::has_variadic(&body.repr.params);
    let has_variadic_named = emit_param::has_variadic_named(&f.params);
    let has_splat = emit_param::has_splat(&body.repr.params);
    body.attrs = get_attrs_for_fun(
        emitter,
        fd,
        &body.attributes,
        false,
        has_variadic,
        has_variadic_named,
        has_splat,
    );
    Ok(Function {
        name: original_id,
        body,
        flags,
    })
}

fn make_memoize_function_code<'a>(
    e: &mut Emitter,
    env: &mut Env<'a>,
    pos: &Pos,
    deprecation_info: Option<&[TypedValue]>,
    hhas_params: &[(Param, Option<(Label, ast::Expr)>)],
    ast_params: &[ast::FunParam],
    renamed_id: hhbc::FunctionName,
    is_async: bool,
    is_reified: bool,
    should_emit_implicit_context: bool,
    should_make_ic_inaccessible: bool,
) -> Result<(InstrSeq, Vec<StringId>)> {
    let (fun, decl_vars) = if hhas_params.is_empty() && !is_reified && !should_emit_implicit_context
    {
        make_memoize_function_no_params_code(
            e,
            env,
            deprecation_info,
            renamed_id,
            is_async,
            should_make_ic_inaccessible,
        )
    } else {
        make_memoize_function_with_params_code(
            e,
            env,
            pos,
            deprecation_info,
            hhas_params,
            ast_params,
            renamed_id,
            is_async,
            is_reified,
            should_emit_implicit_context,
            should_make_ic_inaccessible,
        )
    }?;
    Ok((emit_pos_then(pos, fun), decl_vars))
}

fn make_memoize_function_with_params_code<'a>(
    e: &mut Emitter,
    env: &mut Env<'a>,
    pos: &Pos,
    deprecation_info: Option<&[TypedValue]>,
    hhas_params: &[(Param, Option<(Label, ast::Expr)>)],
    ast_params: &[ast::FunParam],
    renamed_id: hhbc::FunctionName,
    is_async: bool,
    is_reified: bool,
    should_emit_implicit_context: bool,
    should_make_ic_inaccessible: bool,
) -> Result<(InstrSeq, Vec<StringId>)> {
    let param_count = hhas_params.len();
    let notfound = e.label_gen_mut().next_regular();
    let suspended_get = e.label_gen_mut().next_regular();
    let eager_set = e.label_gen_mut().next_regular();
    // The local that contains the reified generics is the first non parameter local,
    // so the first unnamed local is parameter count + 1 when there are reified generics.
    let add_reified = usize::from(is_reified);
    let add_implicit_context = usize::from(should_emit_implicit_context);
    let generics_local = Local::new(param_count); // only used if is_reified == true.
    let decl_vars = match is_reified {
        true => vec![hhbc::intern(reified::GENERICS_LOCAL_NAME)],
        false => Vec::new(),
    };
    e.init_named_locals(
        hhas_params
            .iter()
            .map(|(param, _)| param.name)
            .chain(decl_vars.iter().copied()),
    );
    let first_unnamed_idx = param_count + add_reified;
    let deprecation_body =
        emit_body::emit_deprecation_info(&env.scope, deprecation_info, e.systemlib())?;
    let (begin_label, default_value_setters) =
        // Default value setters belong in the wrapper method not in the original method
        emit_param::emit_param_default_value_setter(e, env, hhas_params, ast_params)?;
    let fcall_args = {
        let mut fcall_flags = FCallArgsFlags::default();
        fcall_flags.set(FCallArgsFlags::HasGenerics, is_reified);
        FCallArgs::new(
            fcall_flags,
            1,
            param_count as u32,
            vec![],
            vec![],
            emit_memoize_helpers::named_arg_names(hhas_params),
            if is_async { Some(eager_set) } else { None },
            None,
        )
    };
    let (reified_get, reified_memokeym) = if !is_reified {
        (instr::empty(), instr::empty())
    } else {
        (
            instr::c_get_l(generics_local),
            InstrSeq::gather(emit_memoize_helpers::get_memo_key_list(
                Local::new(param_count + first_unnamed_idx),
                generics_local,
            )),
        )
    };
    let ic_memokey = if !should_emit_implicit_context {
        instr::empty()
    } else {
        // Last unnamed local slot
        let local = Local::new(first_unnamed_idx + param_count + add_reified);
        emit_memoize_helpers::get_implicit_context_memo_key(local)
    };
    let first_unnamed_local = Local::new(first_unnamed_idx);
    let key_count = (param_count + add_reified + add_implicit_context) as isize;
    let local_range = LocalRange {
        start: first_unnamed_local,
        len: key_count.try_into().unwrap(),
    };
    let ic_stash_local = Local::new(first_unnamed_idx + (key_count) as usize);
    let instrs = InstrSeq::gather(vec![
        begin_label,
        emit_body::emit_method_prolog(e, env, pos, hhas_params, ast_params, &[])?,
        deprecation_body,
        emit_memoize_helpers::param_code_sets(hhas_params.len(), Local::new(first_unnamed_idx)),
        reified_memokeym,
        ic_memokey,
        if is_async {
            InstrSeq::gather(vec![
                instr::memo_get_eager(notfound, suspended_get, local_range),
                instr::ret_c(VerifyRetKind::None),
                instr::label(suspended_get),
                instr::ret_c_suspended(),
            ])
        } else {
            InstrSeq::gather(vec![
                instr::memo_get(notfound, local_range),
                instr::ret_c(VerifyRetKind::None),
            ])
        },
        instr::label(notfound),
        instr::null_uninit(),
        instr::null_uninit(),
        emit_memoize_helpers::param_code_gets(hhas_params.len()),
        reified_get,
        emit_memoize_helpers::with_possible_ic(
            e.label_gen_mut(),
            ic_stash_local,
            InstrSeq::gather(vec![
                instr::f_call_func_d(fcall_args, renamed_id),
                instr::memo_set(local_range),
            ]),
            should_make_ic_inaccessible,
        ),
        if is_async {
            InstrSeq::gather(vec![
                instr::ret_c_suspended(),
                instr::label(eager_set),
                instr::memo_set_eager(local_range),
                emit_memoize_helpers::ic_restore(ic_stash_local, should_make_ic_inaccessible),
                instr::ret_c(VerifyRetKind::None),
            ])
        } else {
            instr::ret_c(VerifyRetKind::None)
        },
        default_value_setters,
    ]);
    Ok((instrs, decl_vars))
}

fn make_memoize_function_no_params_code<'a>(
    e: &mut Emitter,
    env: &mut Env<'a>,
    deprecation_info: Option<&[TypedValue]>,
    renamed_id: hhbc::FunctionName,
    is_async: bool,
    should_make_ic_inaccessible: bool,
) -> Result<(InstrSeq, Vec<StringId>)> {
    let notfound = e.label_gen_mut().next_regular();
    let suspended_get = e.label_gen_mut().next_regular();
    let eager_set = e.label_gen_mut().next_regular();
    let deprecation_body =
        emit_body::emit_deprecation_info(&env.scope, deprecation_info, e.systemlib())?;
    let fcall_args = FCallArgs::new(
        FCallArgsFlags::default(),
        1,
        0,
        vec![],
        vec![],
        vec![],
        if is_async { Some(eager_set) } else { None },
        None,
    );
    let ic_stash_local = Local::new(0);
    let instrs = InstrSeq::gather(vec![
        deprecation_body,
        if is_async {
            InstrSeq::gather(vec![
                instr::memo_get_eager(notfound, suspended_get, LocalRange::EMPTY),
                instr::ret_c(VerifyRetKind::None),
                instr::label(suspended_get),
                instr::ret_c_suspended(),
            ])
        } else {
            InstrSeq::gather(vec![
                instr::memo_get(notfound, LocalRange::EMPTY),
                instr::ret_c(VerifyRetKind::None),
            ])
        },
        instr::label(notfound),
        instr::null_uninit(),
        instr::null_uninit(),
        emit_memoize_helpers::with_possible_ic(
            e.label_gen_mut(),
            ic_stash_local,
            InstrSeq::gather(vec![
                instr::f_call_func_d(fcall_args, renamed_id),
                instr::memo_set(LocalRange::EMPTY),
            ]),
            should_make_ic_inaccessible,
        ),
        if is_async {
            InstrSeq::gather(vec![
                instr::ret_c_suspended(),
                instr::label(eager_set),
                instr::memo_set_eager(LocalRange::EMPTY),
                emit_memoize_helpers::ic_restore(ic_stash_local, should_make_ic_inaccessible),
                instr::ret_c(VerifyRetKind::None),
            ])
        } else {
            instr::ret_c(VerifyRetKind::None)
        },
    ]);
    Ok((instrs, Vec::new()))
}

fn make_wrapper_body<'a>(
    emitter: &mut Emitter,
    env: Env<'a>,
    return_type: TypeInfo,
    attributes: Vec<Attribute>,
    coeffects: Coeffects,
    params: Vec<(Param, Option<(Label, ast::Expr)>)>,
    decl_vars: Vec<StringId>,
    instrs: InstrSeq,
    span: Span,
    tparam_info: Vec<TParamInfo>,
) -> Result<Body> {
    emit_body::make_body(
        emitter,
        instrs,
        decl_vars,
        true,   /* is_memoize_wrapper */
        false,  /* is_memoize_wrapper_lsb */
        vec![], /* upper_bounds */
        tparam_info,
        attributes,
        Attr::AttrNone,
        coeffects,
        params,
        Some(return_type),
        None, /* doc comment */
        Some(&env),
        span,
    )
}
