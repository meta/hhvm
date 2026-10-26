// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use ast_scope::Scope;
use ast_scope::ScopeItem;
use bitflags::bitflags;
use emit_method::get_attrs_for_method;
use emit_pos::emit_pos_then;
use env::Env;
use env::emitter::Emitter;
use error::Error;
use error::Result;
use hhbc::Attr;
use hhbc::Attribute;
use hhbc::Body;
use hhbc::ClassName;
use hhbc::Coeffects;
use hhbc::FCallArgs;
use hhbc::FCallArgsFlags;
use hhbc::Label;
use hhbc::Local;
use hhbc::LocalRange;
use hhbc::Method;
use hhbc::MethodFlags;
use hhbc::Param;
use hhbc::Span;
use hhbc::SpecialClsRef;
use hhbc::StringId;
use hhbc::TParamInfo;
use hhbc::TypeInfo;
use hhbc::TypedValue;
use hhbc::VerifyRetKind;
use hhbc::Visibility;
use hhbc_string_utils::reified;
use instruction_sequence::InstrSeq;
use instruction_sequence::instr;
use naming_special_names_rust::members;
use naming_special_names_rust::user_attributes;
use oxidized::ast;
use oxidized::pos::Pos;

use crate::emit_attribute;
use crate::emit_body;
use crate::emit_memoize_helpers;
use crate::emit_method;
use crate::emit_param;

/// Precomputed information required for generation of memoized methods
pub struct MemoizeInfo {
    /// True if the enclosing class is a trait
    is_trait: bool,
    /// Enclosing class name
    class_name: hhbc::ClassName,
}

fn is_memoize(method: &ast::Method_) -> bool {
    method
        .user_attributes
        .iter()
        .any(|a| user_attributes::is_memoized(&a.name.1))
}

fn is_memoize_lsb(method: &ast::Method_) -> bool {
    method
        .user_attributes
        .iter()
        .any(|a| user_attributes::MEMOIZE_LSB == a.name.1)
}

pub fn make_info(
    class: &ast::Class_,
    class_name: hhbc::ClassName,
    methods: &[ast::Method_],
) -> Result<MemoizeInfo> {
    for m in methods.iter() {
        // check methods
        if is_memoize(m) {
            emit_memoize_helpers::check_memoize_possible(&m.name.0, &m.params[..], true)?;
            let pos = &m.name.0;
            if class.kind.is_cinterface() {
                return Err(Error::fatal_runtime(
                    pos,
                    "<<__Memoize>> cannot be used in interfaces",
                ));
            } else if m.abstract_ {
                return Err(Error::fatal_parse(
                    pos,
                    format!(
                        "Abstract method {}::{} cannot be memoized",
                        class_name.as_str(),
                        &m.name.1,
                    ),
                ));
            };
        };
    }

    let is_trait = class.kind.is_ctrait();
    Ok(MemoizeInfo {
        is_trait,
        class_name,
    })
}

pub fn emit_wrapper_methods<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    info: &MemoizeInfo,
    class: &'a ast::Class_,
    methods: &'a [ast::Method_],
) -> Result<Vec<Method>> {
    // Wrapper methods may not have iterators
    emitter.iterator_mut().reset();

    let mut hhas_methods = vec![];
    for m in methods.iter() {
        if is_memoize(m) {
            env.scope
                .push_item(ScopeItem::Method(ast_scope::Method::new_ref(m)));
            hhas_methods.push(make_memoize_wrapper_method(emitter, env, info, class, m)?);
        };
    }
    Ok(hhas_methods)
}

// This is cut-and-paste from emit_method, with special casing for wrappers
fn make_memoize_wrapper_method<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    info: &MemoizeInfo,
    class: &'a ast::Class_,
    method: &'a ast::Method_,
) -> Result<Method> {
    let ret = if method.name.1 == members::__CONSTRUCT {
        None
    } else {
        method.ret.1.as_ref()
    };
    let name = hhbc::MethodName::intern(&method.name.1);
    let mut scope = Scope::default();
    scope.push_item(ScopeItem::Class(ast_scope::Class::new_ref(class)));
    scope.push_item(ScopeItem::Method(ast_scope::Method::new_ref(method)));
    let mut attributes = emit_attribute::from_asts(emitter, &method.user_attributes)?;
    attributes.extend(emit_attribute::add_reified_attribute(&method.tparams));
    let is_async = method.fun_kind.is_fasync();
    // __Memoize is not allowed on lambdas, so we never need to inherit the rx
    // level from the declaring scope when we're in a Memoize wrapper
    let coeffects = Coeffects::from_ast(
        method.ctxs.as_ref(),
        &method.params,
        &method.tparams,
        &class.tparams,
    );
    let is_reified = method
        .tparams
        .iter()
        .any(|tp| tp.reified.is_reified() || tp.reified.is_soft_reified());
    let mut arg_flags = Flags::empty();
    arg_flags.set(Flags::IS_ASYNC, is_async);
    arg_flags.set(Flags::IS_REIFIED, is_reified);
    arg_flags.set(
        Flags::SHOULD_EMIT_IMPLICIT_CONTEXT,
        hhbc::is_keyed_by_ic_memoize(attributes.iter()),
    );
    arg_flags.set(
        Flags::IS_IC_INACCESSIBLE_SPECIAL_CASE,
        hhbc::is_not_keyed_by_ic_and_leak_ic(attributes.iter()),
    );
    // Match the impl's param layout, which emit_body reorders.
    let ast_params = emit_body::reorder_params(&method.params);
    let mut args = Args {
        info,
        method,
        scope: &scope,
        emit_deprecation_info: true,
        params: &ast_params,
        ret,
        method_id: &name,
        flags: arg_flags,
    };
    let span = Span::from_pos(&method.span);
    let mut body = emit_memoize_wrapper_body(emitter, env, &mut args, span, attributes, coeffects)?;
    let mut flags = MethodFlags::empty();
    flags.set(MethodFlags::IS_ASYNC, is_async);

    let has_variadic = emit_param::has_variadic(&body.repr.params);
    let has_variadic_named = emit_param::has_variadic_named(&method.params);
    let has_splat = emit_param::has_splat(&body.repr.params);
    body.attrs = get_attrs_for_method(
        emitter,
        method,
        &body.attributes,
        &method.visibility,
        class,
        false,
        has_variadic,
        has_variadic_named,
        has_splat,
    );
    Ok(Method {
        visibility: Visibility::from(method.visibility),
        name,
        body,
        flags,
    })
}

fn emit_memoize_wrapper_body<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    args: &mut Args<'_, 'a>,
    span: Span,
    attributes: Vec<Attribute>,
    coeffects: Coeffects,
) -> Result<Body> {
    let mut tparams: Vec<&str> = args
        .scope
        .get_tparams()
        .iter()
        .map(|tp| tp.name.1.as_str())
        .collect();
    let tparam_info = args
        .scope
        .get_fun_tparams()
        .iter()
        .map(|tp| TParamInfo {
            name: ClassName::intern(tp.name.1.as_str()),
            shadows_class_tparam: false,
        })
        .collect::<Vec<_>>();
    let return_type =
        emit_body::emit_return_type(&tparams[..], args.flags.contains(Flags::IS_ASYNC), args.ret)?;
    let hhas_params = emit_param::from_asts(emitter, &mut tparams, true, args.scope, args.params)?;
    args.flags.set(Flags::WITH_LSB, is_memoize_lsb(args.method));
    args.flags.set(Flags::IS_STATIC, args.method.static_);
    emit(
        emitter,
        env,
        hhas_params,
        return_type,
        span,
        attributes,
        coeffects,
        args,
        tparam_info,
    )
}

fn emit<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    hhas_params: Vec<(Param, Option<(Label, ast::Expr)>)>,
    return_type: TypeInfo,
    span: Span,
    attributes: Vec<Attribute>,
    coeffects: Coeffects,
    args: &Args<'_, 'a>,
    tparam_info: Vec<TParamInfo>,
) -> Result<Body> {
    let pos = &args.method.span;
    let depr_info = match args.emit_deprecation_info {
        true => hhbc::deprecation_info(&attributes),
        false => None,
    };
    let (instrs, decl_vars) =
        make_memoize_method_code(emitter, env, pos, &hhas_params, args, depr_info, &coeffects)?;
    let instrs = emit_pos_then(pos, instrs);
    make_wrapper(
        emitter,
        env,
        instrs,
        hhas_params,
        decl_vars,
        return_type,
        span,
        attributes,
        coeffects,
        args,
        tparam_info,
    )
}

fn make_memoize_method_code<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    pos: &Pos,
    hhas_params: &[(Param, Option<(Label, ast::Expr)>)],
    args: &Args<'_, 'a>,
    deprecation_info: Option<&[TypedValue]>,
    coeffects: &Coeffects,
) -> Result<(InstrSeq, Vec<StringId>)> {
    if args.params.is_empty()
        && !args.flags.contains(Flags::IS_REIFIED)
        && !args.flags.contains(Flags::SHOULD_EMIT_IMPLICIT_CONTEXT)
    {
        make_memoize_method_no_params_code(emitter, args, deprecation_info, coeffects)
    } else {
        make_memoize_method_with_params_code(
            emitter,
            env,
            pos,
            hhas_params,
            args,
            deprecation_info,
            coeffects,
        )
    }
}

// method is the already-renamed memoize method that must be wrapped
fn make_memoize_method_with_params_code<'a>(
    emitter: &mut Emitter,
    env: &mut Env<'a>,
    pos: &Pos,
    hhas_params: &[(Param, Option<(Label, ast::Expr)>)],
    args: &Args<'_, 'a>,
    deprecation_info: Option<&[TypedValue]>,
    coeffects: &Coeffects,
) -> Result<(InstrSeq, Vec<StringId>)> {
    let param_count = hhas_params.len();
    let notfound = emitter.label_gen_mut().next_regular();
    let suspended_get = emitter.label_gen_mut().next_regular();
    let eager_set = emitter.label_gen_mut().next_regular();
    // The local that contains the reified generics is the first non parameter local,
    // so the first unnamed local is parameter count + 1 when there are reified generics.
    let is_reified = args.flags.contains(Flags::IS_REIFIED);
    let add_reified = usize::from(is_reified);
    // #NotKeyedByICAndLeakIC__DO_NOT_USE won't shard by IC but will access IC
    let is_not_keyed_by_ic_and_leak_ic =
        args.flags.contains(Flags::IS_IC_INACCESSIBLE_SPECIAL_CASE);
    let should_emit_implicit_context = args.flags.contains(Flags::SHOULD_EMIT_IMPLICIT_CONTEXT);
    let add_implicit_context = usize::from(should_emit_implicit_context);
    let first_unnamed_idx = param_count + add_reified;
    let generics_local = Local::new(param_count); // only used if is_reified == true.
    let decl_vars = match is_reified {
        true => vec![hhbc::intern(reified::GENERICS_LOCAL_NAME)],
        false => Vec::new(),
    };
    emitter.init_named_locals(
        hhas_params
            .iter()
            .map(|(param, _)| param.name)
            .chain(decl_vars.iter().copied()),
    );
    let deprecation_body =
        emit_body::emit_deprecation_info(args.scope, deprecation_info, emitter.systemlib())?;
    let (begin_label, default_value_setters) =
        // Default value setters belong in the wrapper method not in the original method
        emit_param::emit_param_default_value_setter(emitter, env, hhas_params, args.params)?;
    let fcall_args = {
        let mut fcall_flags = FCallArgsFlags::default();
        if is_reified {
            fcall_flags |= FCallArgsFlags::HasGenerics;
        };
        let async_eager_target = if args.flags.contains(Flags::IS_ASYNC) {
            Some(eager_set)
        } else {
            None
        };
        FCallArgs::new(
            fcall_flags,
            1,
            param_count as u32,
            vec![],
            vec![],
            emit_memoize_helpers::named_arg_names(hhas_params),
            async_eager_target,
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
    let ic_stash_local = Local::new((key_count) as usize + first_unnamed_idx);
    // This fn either has IC unoptimizable static coeffects, or has any dynamic coeffects
    let has_ic_unoptimizable_coeffects: bool = coeffects.has_ic_unoptimizable_coeffects();
    let should_make_ic_inaccessible: bool = !is_not_keyed_by_ic_and_leak_ic
        && !should_emit_implicit_context
        && has_ic_unoptimizable_coeffects;
    let instrs = InstrSeq::gather(vec![
        begin_label,
        emit_body::emit_method_prolog(emitter, env, pos, hhas_params, args.params, &[])?,
        deprecation_body,
        if args.method.static_ {
            instr::empty()
        } else {
            instr::check_this()
        },
        emit_memoize_helpers::param_code_sets(hhas_params.len(), Local::new(first_unnamed_idx)),
        reified_memokeym,
        ic_memokey,
        if args.flags.contains(Flags::IS_ASYNC) {
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
        if args.method.static_ {
            instr::null_uninit()
        } else {
            instr::this()
        },
        instr::null_uninit(),
        emit_memoize_helpers::param_code_gets(hhas_params.len()),
        reified_get,
        emit_memoize_helpers::with_possible_ic(
            emitter.label_gen_mut(),
            ic_stash_local,
            InstrSeq::gather(vec![
                if args.method.static_ {
                    call_cls_method(fcall_args, args)
                } else {
                    let renamed_method_id = hhbc::MethodName::add_suffix(
                        args.method_id,
                        emit_memoize_helpers::MEMOIZE_SUFFIX,
                    );
                    instr::f_call_obj_method_d(fcall_args, renamed_method_id)
                },
                instr::memo_set(local_range),
            ]),
            should_make_ic_inaccessible,
        ),
        if args.flags.contains(Flags::IS_ASYNC) {
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

fn make_memoize_method_no_params_code<'a>(
    emitter: &mut Emitter,
    args: &Args<'_, 'a>,
    deprecation_info: Option<&[TypedValue]>,
    coeffects: &Coeffects,
) -> Result<(InstrSeq, Vec<StringId>)> {
    let notfound = emitter.label_gen_mut().next_regular();
    let suspended_get = emitter.label_gen_mut().next_regular();
    let eager_set = emitter.label_gen_mut().next_regular();
    let deprecation_body =
        emit_body::emit_deprecation_info(args.scope, deprecation_info, emitter.systemlib())?;

    let fcall_args = FCallArgs::new(
        FCallArgsFlags::default(),
        1,
        0,
        vec![],
        vec![],
        vec![],
        if args.flags.contains(Flags::IS_ASYNC) {
            Some(eager_set)
        } else {
            None
        },
        None,
    );
    // #NotKeyedByICAndLeakIC__DO_NOT_USE won't shard by IC but will access IC
    let is_not_keyed_by_ic_and_leak_ic =
        args.flags.contains(Flags::IS_IC_INACCESSIBLE_SPECIAL_CASE);
    let ic_stash_local = Local::new(0);
    // we are in a no parameter function that sets no zoned IC either, default to what coeffects suggest
    let should_make_ic_inaccessible: bool =
        coeffects.has_ic_unoptimizable_coeffects() && !is_not_keyed_by_ic_and_leak_ic;
    let instrs = InstrSeq::gather(vec![
        deprecation_body,
        if args.method.static_ {
            instr::empty()
        } else {
            instr::check_this()
        },
        if args.flags.contains(Flags::IS_ASYNC) {
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
        if args.method.static_ {
            instr::null_uninit()
        } else {
            instr::this()
        },
        instr::null_uninit(),
        emit_memoize_helpers::with_possible_ic(
            emitter.label_gen_mut(),
            ic_stash_local,
            InstrSeq::gather(vec![
                if args.method.static_ {
                    call_cls_method(fcall_args, args)
                } else {
                    let renamed_method_id = hhbc::MethodName::add_suffix(
                        args.method_id,
                        emit_memoize_helpers::MEMOIZE_SUFFIX,
                    );
                    instr::f_call_obj_method_d(fcall_args, renamed_method_id)
                },
                instr::memo_set(LocalRange::EMPTY),
            ]),
            should_make_ic_inaccessible,
        ),
        if args.flags.contains(Flags::IS_ASYNC) {
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

// Construct the wrapper function
fn make_wrapper<'a>(
    emitter: &mut Emitter,
    env: &Env<'a>,
    instrs: InstrSeq,
    params: Vec<(Param, Option<(Label, ast::Expr)>)>,
    decl_vars: Vec<StringId>,
    return_type: TypeInfo,
    span: Span,
    attributes: Vec<Attribute>,
    coeffects: Coeffects,
    args: &Args<'_, 'a>,
    tparam_info: Vec<TParamInfo>,
) -> Result<Body> {
    emit_body::make_body(
        emitter,
        instrs,
        decl_vars,
        true,
        args.flags.contains(Flags::WITH_LSB),
        vec![], /* upper_bounds */
        tparam_info,
        attributes,
        Attr::AttrNone,
        coeffects,
        params,
        Some(return_type),
        None,
        Some(env),
        span,
    )
}

fn call_cls_method<'a>(fcall_args: FCallArgs, args: &Args<'_, 'a>) -> InstrSeq {
    let method_id =
        hhbc::MethodName::add_suffix(args.method_id, emit_memoize_helpers::MEMOIZE_SUFFIX);
    if args.info.is_trait || args.flags.contains(Flags::WITH_LSB) {
        instr::f_call_cls_method_sd(fcall_args, SpecialClsRef::SelfCls, method_id)
    } else {
        instr::f_call_cls_method_d(fcall_args, method_id, args.info.class_name)
    }
}

struct Args<'r, 'ast> {
    pub info: &'r MemoizeInfo,
    pub method: &'r ast::Method_,
    pub scope: &'r Scope<'ast>,
    pub emit_deprecation_info: bool,
    pub params: &'r [ast::FunParam],
    pub ret: Option<&'r ast::Hint>,
    pub method_id: &'r hhbc::MethodName,
    pub flags: Flags,
}

bitflags! {
    #[derive(PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Clone, Copy)]
    pub struct Flags: u8 {
        const IS_STATIC = 1 << 1;
        const IS_REIFIED = 1 << 2;
        const WITH_LSB = 1 << 3;
        const IS_ASYNC = 1 << 4;
        const SHOULD_EMIT_IMPLICIT_CONTEXT = 1 << 5;
        const SHOULD_MAKE_IC_INACCESSIBLE = 1 << 6;
        const IS_IC_INACCESSIBLE_SPECIAL_CASE = 1 << 7;
    }
}
