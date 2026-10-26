// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::borrow::Cow;
use std::sync::Arc;

use ast_scope::Lambda;
use ast_scope::Scope;
use ast_scope::ScopeItem;
use env::emitter::Emitter;
use error::Error;
use error::Result;
use hhbc::Attribute;
use hhbc::Coeffects;
use hhbc::Method;
use hhbc::MethodFlags;
use hhbc::Span;
use hhbc::Visibility;
use hhbc_string_utils as string_utils;
use hhvm_types_ffi::ffi::Attr;
use instruction_sequence::instr;
use naming_special_names_rust::classes;
use naming_special_names_rust::members;
use naming_special_names_rust::user_attributes;
use oxidized::ast;
use oxidized::ast_defs;

use crate::emit_attribute;
use crate::emit_body;
use crate::emit_fatal;
use crate::emit_memoize_helpers;
use crate::emit_native_opcode;
use crate::emit_param;

pub fn from_asts<'a>(
    emitter: &mut Emitter,
    class: &'a ast::Class_,
    methods: &'a [ast::Method_],
) -> Result<Vec<Method>> {
    methods
        .iter()
        .map(|m| from_ast(emitter, class, m))
        .collect()
}

pub fn get_attrs_for_method(
    emitter: &mut Emitter,
    method: &ast::Method_,
    user_attrs: &[Attribute],
    visibility: &ast::Visibility,
    class: &ast::Class_,
    is_memoize_impl: bool,
    has_variadic: bool,
    has_variadic_named: bool,
    has_splat: bool,
) -> Attr {
    let is_abstract = class.kind.is_cinterface() || method.abstract_;
    let is_systemlib = emitter.systemlib();
    let is_dyn_callable =
        is_systemlib || (hhbc::has_dynamically_callable(user_attrs) && !is_memoize_impl);
    let is_no_injection = hhbc::is_no_injection(user_attrs) || method.hidden;
    let is_prov_skip_frame = hhbc::has_provenance_skip_frame(user_attrs);
    let is_readonly_return = method.readonly_ret.is_some();

    let mut attrs = Attr::AttrNone;
    attrs.add(Attr::from(visibility));
    attrs.set(Attr::AttrAbstract, is_abstract);
    attrs.set(Attr::AttrBuiltin, is_systemlib);
    attrs.set(Attr::AttrDynamicallyCallable, is_dyn_callable);
    attrs.set(Attr::AttrFinal, method.final_);
    attrs.set(Attr::AttrIsFoldable, hhbc::has_foldable(user_attrs));
    attrs.set(Attr::AttrNoInjection, is_no_injection);
    attrs.set(Attr::AttrReadonlyReturn, is_readonly_return);
    attrs.set(Attr::AttrReadonlyThis, method.readonly_this);
    attrs.set(Attr::AttrStatic, method.static_);
    attrs.set(Attr::AttrVariadicParam, has_variadic);
    attrs.set(Attr::AttrVariadicNamedParam, has_variadic_named);
    attrs.set(Attr::AttrSplatParam, has_splat);
    attrs.set(Attr::AttrProvenanceSkipFrame, is_prov_skip_frame);
    attrs
}

pub fn from_ast<'a, 'd>(
    emitter: &mut Emitter,
    class: &'a ast::Class_,
    method_: impl Into<Cow<'a, ast::Method_>>,
) -> Result<Method> {
    let method_: Cow<'a, ast::Method_> = method_.into();
    let method = method_.as_ref();
    let is_memoize = method
        .user_attributes
        .iter()
        .any(|ua| user_attributes::is_memoized(&ua.name.1));
    let class_name = string_utils::mangle(string_utils::strip_global_ns(&class.name.1).into());
    let is_closure_body =
        method.name.1 == members::__INVOKE && (class.name.1).starts_with("Closure$");
    let mut attributes = emit_attribute::from_asts(emitter, &method.user_attributes)?;
    if !is_closure_body {
        attributes.extend(emit_attribute::add_reified_attribute(&method.tparams[..]));
    };
    let call_context = if is_closure_body {
        match &method.user_attributes[..] {
            [
                ast::UserAttribute {
                    name: ast_defs::Id(_, s),
                    params: _,
                },
            ] if s.eq_ignore_ascii_case("__DynamicMethCallerForce") => {
                Some(hhbc::intern("__SystemLib\\DynamicContextOverrideUnsafe"))
            }
            _ => None,
        }
    } else {
        None
    };

    let is_native = attributes
        .iter()
        .any(|attr| attr.is(user_attributes::is_native));
    let is_native_opcode_impl = hhbc::is_native_opcode_impl(&attributes);
    let is_async = method.fun_kind.is_fasync() || method.fun_kind.is_fasync_generator();

    if class.kind.is_cinterface() && !method.body.fb_ast.is_empty() {
        return Err(Error::fatal_parse(
            &method.name.0,
            format!(
                "Interface method {}::{} cannot contain body",
                class_name, &method.name.1
            ),
        ));
    };
    let is_cabstract = match class.kind {
        ast_defs::ClassishKind::Cclass(k) => k.is_abstract(),
        _ => false,
    };
    if !method.static_ && class.final_ && is_cabstract {
        return Err(Error::fatal_parse(
            &method.name.0,
            format!(
                "Class {} contains non-static method {} and therefore cannot be declared 'abstract final'",
                class_name, &method.name.1
            ),
        ));
    };

    let visibility = if is_native_opcode_impl {
        ast::Visibility::Public
    } else if is_memoize {
        ast::Visibility::Private
    } else {
        method.visibility
    };
    let default_dropthrough = if method.abstract_ {
        Some(emit_fatal::emit_fatal_runtimeomitframe(
            &method.name.0,
            format!(
                "Cannot call abstract method {}::{}()",
                class_name, &method.name.1
            ),
        ))
    } else {
        None
    };
    let mut scope = Scope::default();
    scope.push_item(ScopeItem::Class(ast_scope::Class::new_ref(class)));
    scope.push_item(ScopeItem::Method(match &method_ {
        Cow::Borrowed(m) => ast_scope::Method::new_ref(m),
        Cow::Owned(m) => ast_scope::Method::new_rc(m),
    }));
    if is_closure_body {
        scope.push_item(ScopeItem::Lambda(Lambda {
            is_long: false,
            is_async,
            coeffects: Coeffects::default(),
            pos: method.span.clone(),
        }))
    };
    let namespace = Arc::clone(
        emitter
            .global_state()
            .closure_namespaces
            .get(&class_name)
            .unwrap_or(&class.namespace),
    );
    let mut coeffects = if method.ctxs.is_none() && is_closure_body {
        let parent_coeffects = emitter
            .global_state()
            .get_lambda_coeffects_of_scope(&class.name.1, &method.name.1);
        parent_coeffects.map_or(Coeffects::default(), |pc| pc.inherit_to_child_closure())
    } else {
        Coeffects::from_ast(
            method.ctxs.as_ref(),
            &method.params,
            &method.tparams,
            &class.tparams,
        )
    };

    if is_closure_body && coeffects.is_86caller() {
        coeffects = coeffects.with_caller()
    }

    if is_native_opcode_impl
        && (class.name.1.as_str() == classes::GENERATOR
            || class.name.1.as_str() == classes::ASYNC_GENERATOR)
    {
        match method.name.1.as_str() {
            "send" | "raise" | "throw" | "next" | "rewind" => {
                coeffects = coeffects.with_gen_coeffect()
            }
            _ => {}
        }
    }
    if emitter.systemlib() {
        match (class.name.1.as_str(), method.name.1.as_str()) {
            ("\\__SystemLib\\MethCallerHelper", members::__INVOKE)
            | ("\\__SystemLib\\DynMethCallerHelper", members::__INVOKE) => {
                coeffects = coeffects.with_caller()
            }
            _ => {}
        }
    }
    let ast_body_block = &method.body.fb_ast;
    let (mut body, is_generator, is_pair_generator) = if is_native_opcode_impl {
        (
            emit_native_opcode::emit_body(
                emitter,
                &scope,
                &class.name,
                &class.user_attributes,
                &method.name,
                &method.params,
                attributes,
                Attr::AttrNone,
                coeffects,
                method.ret.1.as_ref(),
            )?,
            false,
            false,
        )
    } else {
        let class_tparam_names = class
            .tparams
            .iter()
            .map(|tp| (tp.name).1.as_str())
            .collect::<Vec<_>>();
        let mut flags = emit_body::Flags::empty();
        flags.set(
            emit_body::Flags::SKIP_AWAITABLE,
            method.fun_kind.is_fasync(),
        );
        flags.set(emit_body::Flags::MEMOIZE, is_memoize);
        flags.set(emit_body::Flags::CLOSURE_BODY, is_closure_body);
        flags.set(emit_body::Flags::NATIVE, is_native);
        flags.set(emit_body::Flags::ASYNC, is_async);
        flags.set(
            emit_body::Flags::HAS_COEFFECTS_LOCAL,
            coeffects.has_coeffects_local(),
        );
        emit_body::emit_body(
            emitter,
            namespace,
            ast_body_block,
            instr::null(),
            scope,
            Span::from_pos(&method.span),
            attributes,
            Attr::AttrNone,
            coeffects,
            emit_body::Args {
                immediate_tparams: &method.tparams,
                class_tparam_names: class_tparam_names.as_slice(),
                ast_params: &method.params,
                ret: method.ret.1.as_ref(),
                pos: &method.span,
                emit_deprecation_info: !is_memoize,
                doc_comment: method.doc_comment.clone(),
                default_dropthrough,
                call_context,
                flags,
            },
        )?
    };
    let name = {
        if is_memoize {
            hhbc::MethodName::from_ast_name_and_suffix(
                &method.name.1,
                emit_memoize_helpers::MEMOIZE_SUFFIX,
            )
        } else {
            hhbc::MethodName::from_ast_name(&method.name.1)
        }
    };
    let mut flags = MethodFlags::empty();
    flags.set(MethodFlags::IS_ASYNC, is_async);
    flags.set(MethodFlags::IS_GENERATOR, is_generator);
    flags.set(MethodFlags::IS_PAIR_GENERATOR, is_pair_generator);
    flags.set(MethodFlags::IS_CLOSURE_BODY, is_closure_body);

    let has_variadic = emit_param::has_variadic(&body.repr.params);
    let has_variadic_named = emit_param::has_variadic_named(&method.params);
    let has_splat = emit_param::has_splat(&body.repr.params);
    body.attrs = get_attrs_for_method(
        emitter,
        method,
        &body.attributes,
        &visibility,
        class,
        is_memoize,
        has_variadic,
        has_variadic_named,
        has_splat,
    );
    Ok(Method {
        visibility: Visibility::from(visibility),
        name,
        body,
        flags,
    })
}
