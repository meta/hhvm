// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
#![feature(deref_patterns)]
use std::borrow::Cow;
use std::sync::Arc;

use env::emitter::Emitter;
use error::Error;
use error::Result;
use global_state::ClosureEnclosingClassInfo;
use global_state::GlobalState;
use hack_macros::hack_expr;
use hash::IndexSet;
use hhbc::Coeffects;
use hhbc_string_utils as string_utils;
use itertools::Itertools;
use naming_special_names_rust::members;
use naming_special_names_rust::pseudo_consts;
use naming_special_names_rust::special_idents;
use oxidized::aast::UserAttribute;
use oxidized::aast_visitor;
use oxidized::aast_visitor::AstParams;
use oxidized::aast_visitor::NodeMut;
use oxidized::aast_visitor::VisitorMut;
use oxidized::aast_visitor::visit_mut;
use oxidized::ast::Abstraction;
use oxidized::ast::Block;
use oxidized::ast::CallExpr;
use oxidized::ast::CaptureLid;
use oxidized::ast::Class_;
use oxidized::ast::ClassHint;
use oxidized::ast::ClassId_;
use oxidized::ast::ClassName;
use oxidized::ast::ClassVar;
use oxidized::ast::ClassishKind;
use oxidized::ast::Contexts;
use oxidized::ast::Def;
use oxidized::ast::Efun;
use oxidized::ast::EmitId;
use oxidized::ast::Expr;
use oxidized::ast::Expr_;
use oxidized::ast::Fun_;
use oxidized::ast::FunDef;
use oxidized::ast::FunKind;
use oxidized::ast::FunParam;
use oxidized::ast::FunParamInfo;
use oxidized::ast::FuncBody;
use oxidized::ast::Hint;
use oxidized::ast::Hint_;
use oxidized::ast::Id;
use oxidized::ast::Lid;
use oxidized::ast::LocalId;
use oxidized::ast::LoopCond;
use oxidized::ast::LoopIter;
use oxidized::ast::Method_;
use oxidized::ast::Pos;
use oxidized::ast::ReifyKind;
use oxidized::ast::Sid;
use oxidized::ast::Stmt;
use oxidized::ast::Stmt_;
use oxidized::ast::Tparam;
use oxidized::ast::TypeHint;
use oxidized::ast::UserAttributes;
use oxidized::ast::Variance;
use oxidized::ast::Visibility;
use oxidized::ast_defs::ParamKind;
use oxidized::ast_defs::PropOrMethod;
use oxidized::file_info::Mode;
use oxidized::local_id;
use oxidized::namespace_env;
use oxidized::s_map::SMap;
use unique_id_builder::get_unique_id_for_function;
use unique_id_builder::get_unique_id_for_main;
use unique_id_builder::get_unique_id_for_method;

#[derive(Default)]
struct Variables {
    /// all variables declared/used in the scope
    all_vars: IndexSet<String>,
    /// names of parameters if scope correspond to a function
    parameter_names: IndexSet<String>,
    /// If this is a long lambda then the list of explicitly captured vars (if
    /// any).
    explicit_capture: IndexSet<String>,
}

struct ClassSummary<'b> {
    extends: &'b [ClassHint],
    kind: ClassishKind,
    mode: Mode,
    name: &'b ClassName,
    span: &'b Pos,
    tparams: &'b [Tparam],
}

struct FunctionSummary<'b> {
    // Unfortunately hhbc::Coeffects has to be owned for now.
    coeffects: Coeffects,
    fun_kind: FunKind,
    mode: Mode,
    name: &'b Sid,
    span: &'b Pos,
    tparams: &'b [Tparam],
    hidden: bool,
}

struct LambdaSummary<'b> {
    // Unfortunately hhbc::Coeffects has to be owned for now.
    coeffects: Coeffects,
    explicit_capture: Option<&'b [CaptureLid]>,
    fun_kind: FunKind,
    span: &'b Pos,
    hidden: bool,
}

struct MethodSummary<'b> {
    // Unfortunately hhbc::Coeffects has to be owned for now.
    coeffects: Coeffects,
    fun_kind: FunKind,
    name: &'b Sid,
    span: &'b Pos,
    static_: bool,
    tparams: &'b [Tparam],
    hidden: bool,
}

enum ScopeSummary<'b> {
    TopLevel,
    Class(ClassSummary<'b>),
    Function(FunctionSummary<'b>),
    Method(MethodSummary<'b>),
    Lambda(LambdaSummary<'b>),
}

impl<'b> ScopeSummary<'b> {
    fn span(&self) -> Option<&Pos> {
        match self {
            ScopeSummary::TopLevel => None,
            ScopeSummary::Class(cd) => Some(cd.span),
            ScopeSummary::Function(fd) => Some(fd.span),
            ScopeSummary::Method(md) => Some(md.span),
            ScopeSummary::Lambda(ld) => Some(ld.span),
        }
    }

    fn hidden(&self) -> bool {
        match self {
            ScopeSummary::TopLevel => false,
            ScopeSummary::Class(_) => false,
            ScopeSummary::Function(fd) => fd.hidden,
            ScopeSummary::Method(md) => md.hidden,
            ScopeSummary::Lambda(ld) => ld.hidden,
        }
    }
}

/// The environment for a scope. This tracks the summary information and
/// variables used within a scope.
struct Scope<'b> {
    parent: Option<&'b Scope<'b>>,

    /// Number of closures created in the current function
    closure_cnt_per_fun: u32,
    /// Are we immediately in a using statement?
    in_using: bool,
    /// What is the current context?
    summary: ScopeSummary<'b>,
    /// What variables are defined in this scope?
    variables: Variables,
    /// Is this scope currently within a pipe-expression RHS
    has_pipe: bool,
}

impl<'b> Scope<'b> {
    fn toplevel(defs: &[Def]) -> Result<Self> {
        let all_vars = compute_vars(&[], &defs)?;

        Ok(Self {
            in_using: false,
            closure_cnt_per_fun: 0,
            parent: None,
            summary: ScopeSummary::TopLevel,
            variables: Variables {
                all_vars,
                ..Variables::default()
            },
            has_pipe: false,
        })
    }

    fn as_class_summary(&self) -> Option<&ClassSummary<'_>> {
        self.walk_scope().find_map(|scope| match &scope.summary {
            ScopeSummary::Class(cd) => Some(cd),
            _ => None,
        })
    }

    fn check_if_in_async_context(&self) -> Result<()> {
        let check_valid_fun_kind = |name, kind: FunKind| {
            if !kind.is_async() {
                Err(Error::fatal_parse(
                    &self.span_or_none(),
                    format!(
                        "Function '{}' contains 'await' but is not declared as async.",
                        string_utils::strip_global_ns(name)
                    ),
                ))
            } else {
                Ok(())
            }
        };
        let check_lambda = |is_async: bool| {
            if !is_async {
                Err(Error::fatal_parse(
                    &self.span_or_none(),
                    "Await may only appear in an async function",
                ))
            } else {
                Ok(())
            }
        };
        match &self.summary {
            ScopeSummary::TopLevel => Err(Error::fatal_parse(
                &self.span_or_none(),
                "'await' can only be used inside a function",
            )),
            ScopeSummary::Lambda(ld) => check_lambda(ld.fun_kind.is_async()),
            ScopeSummary::Class(_) => Ok(()), /* Syntax error, wont get here */
            ScopeSummary::Function(fd) => check_valid_fun_kind(&fd.name.1, fd.fun_kind),
            ScopeSummary::Method(md) => check_valid_fun_kind(&md.name.1, md.fun_kind),
        }
    }

    fn class_tparams(&self) -> &[Tparam] {
        self.as_class_summary().map_or(&[], |cd| cd.tparams)
    }

    fn coeffects_of_scope<'c>(&'c self) -> Option<&'c Coeffects> {
        for scope in self.walk_scope() {
            match &scope.summary {
                ScopeSummary::Class(_) => break,
                ScopeSummary::Method(md) => return Some(&md.coeffects),
                ScopeSummary::Function(fd) => return Some(&fd.coeffects),
                ScopeSummary::Lambda(ld) => {
                    if !ld.coeffects.get_static_coeffects().is_empty() {
                        return Some(&ld.coeffects);
                    }
                }
                ScopeSummary::TopLevel => {}
            }
        }
        None
    }

    fn fun_tparams(&self) -> &[Tparam] {
        for scope in self.walk_scope() {
            match &scope.summary {
                ScopeSummary::Class(_) => break,
                ScopeSummary::Function(fd) => return fd.tparams,
                ScopeSummary::Method(md) => return md.tparams,
                _ => {}
            }
        }
        &[]
    }

    fn is_in_debugger_eval_fun(&self) -> bool {
        let mut cur = Some(self);
        while let Some(scope) = cur {
            match &scope.summary {
                ScopeSummary::Lambda(_) => {}
                ScopeSummary::Function(fd) => return fd.name.1 == "include",
                _ => break,
            }
            cur = scope.parent;
        }
        false
    }

    fn is_static(&self) -> bool {
        for scope in self.walk_scope() {
            match &scope.summary {
                ScopeSummary::Function(_) => return true,
                ScopeSummary::Method(md) => return md.static_,
                ScopeSummary::Lambda(_) => {}
                ScopeSummary::Class(_) | ScopeSummary::TopLevel => unreachable!(),
            }
        }
        unreachable!();
    }

    fn make_scope_name(&self, ns: &Arc<namespace_env::Env>) -> String {
        let mut parts = Vec::new();
        let mut iter = self.walk_scope();
        while let Some(scope) = iter.next() {
            match &scope.summary {
                ScopeSummary::Class(cd) => {
                    parts.push(make_class_name(cd.name));
                    break;
                }
                ScopeSummary::Function(fd) => {
                    let fname = strip_id(fd.name);
                    parts.push(fname.into());
                    for sub_scope in iter {
                        match &sub_scope.summary {
                            ScopeSummary::Class(cd) => {
                                parts.push("::".into());
                                parts.push(make_class_name(cd.name));
                                break;
                            }
                            _ => {}
                        }
                    }
                    break;
                }
                ScopeSummary::Method(x) => {
                    parts.push(strip_id(x.name).to_string());
                    if !parts.last().is_some_and(|x| x.ends_with("::")) {
                        parts.push("::".into())
                    };
                }
                ScopeSummary::Lambda(_) | ScopeSummary::TopLevel => {}
            }
        }

        if parts.is_empty() {
            if let Some(n) = &ns.name {
                format!("{}\\", n)
            } else {
                String::new()
            }
        } else {
            parts.reverse();
            parts.join("")
        }
    }

    /// Create a new Env which uses `self` as its parent.
    fn new_child<'s>(
        &'s self,
        summary: ScopeSummary<'s>,
        variables: Variables,
    ) -> Result<Scope<'s>> {
        Ok(Scope {
            in_using: self.in_using,
            closure_cnt_per_fun: self.closure_cnt_per_fun,
            parent: Some(self),
            summary,
            variables,
            has_pipe: self.has_pipe,
        })
    }

    fn scope_fmode(&self) -> Mode {
        for scope in self.walk_scope() {
            match &scope.summary {
                ScopeSummary::Class(cd) => return cd.mode,
                ScopeSummary::Function(fd) => return fd.mode,
                _ => {}
            }
        }
        Mode::Mstrict
    }

    fn should_capture_var(&self, var: &str) -> bool {
        // variable used in lambda should be captured if is:
        // - not contained in lambda parameter list
        if self.variables.parameter_names.contains(var) {
            return false;
        };
        // AND
        // - it exists in one of enclosing scopes
        for scope in self.walk_scope().skip(1) {
            let vars = &scope.variables;
            if vars.all_vars.contains(var)
                || vars.parameter_names.contains(var)
                || vars.explicit_capture.contains(var)
            {
                return true;
            }

            if var == special_idents::DOLLAR_DOLLAR && scope.has_pipe {
                return true;
            }

            match &scope.summary {
                ScopeSummary::Lambda(ld) => {
                    // A lambda contained within an anonymous function (a 'long'
                    // lambda) shouldn't capture variables from outside the
                    // anonymous function unless they're explicitly mentioned in
                    // the function's use clause.
                    if ld.explicit_capture.is_some() {
                        return false;
                    }
                }
                ScopeSummary::TopLevel => {}
                _ => return false,
            }
        }

        false
    }

    fn span(&self) -> Option<&Pos> {
        self.summary.span()
    }

    fn span_or_none<'s>(&'s self) -> Cow<'s, Pos> {
        if let Some(pos) = self.span() {
            Cow::Borrowed(pos)
        } else {
            Cow::Owned(Pos::NONE)
        }
    }

    fn walk_scope<'s>(&'s self) -> ScopeIter<'b, 's> {
        ScopeIter(Some(self))
    }

    fn with_in_using<F, R>(&mut self, in_using: bool, mut f: F) -> R
    where
        F: FnMut(&mut Self) -> R,
    {
        let old_in_using = self.in_using;
        self.in_using = in_using;
        let r = f(self);
        self.in_using = old_in_using;
        r
    }

    fn with_in_pipe<F, R>(&mut self, mut f: F) -> R
    where
        F: FnMut(&mut Self) -> R,
    {
        let old = self.has_pipe;
        self.has_pipe = true;
        let r = f(self);
        self.has_pipe = old;
        r
    }
}

struct ScopeIter<'b, 'c>(Option<&'c Scope<'b>>);

impl<'b, 'c> Iterator for ScopeIter<'b, 'c> {
    type Item = &'c Scope<'c>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(cur) = self.0.take() {
            self.0 = cur.parent;
            Some(cur)
        } else {
            None
        }
    }
}

#[derive(Clone, Default)]
struct CaptureState {
    this_: bool,
    // Free variables computed so far
    vars: IndexSet<String>,
    generics: IndexSet<String>,
}

/// ReadOnlyState is split from State because it can be a simple ref in
/// ClosureVisitor.
struct ReadOnlyState {
    // Empty namespace as constructed by parser
    empty_namespace: Arc<namespace_env::Env>,
    /// For debugger eval
    for_debugger_eval: bool,
}

/// Mutable state used during visiting in ClosureVisitor. It's mutable and owned
/// so it needs to be moved as we push and pop scopes.
struct State {
    capture_state: CaptureState,
    // Closure classes
    closures: Vec<Class_>,
    // accumulated information about program
    global_state: GlobalState,
    /// Hoisted meth_caller functions
    named_hoisted_functions: SMap<FunDef>,
    // The current namespace environment
    namespace: Arc<namespace_env::Env>,
}

impl State {
    fn initial_state(empty_namespace: Arc<namespace_env::Env>) -> Self {
        Self {
            capture_state: Default::default(),
            closures: vec![],
            global_state: GlobalState::default(),
            named_hoisted_functions: SMap::new(),
            namespace: empty_namespace,
        }
    }

    fn record_function_state(&mut self, key: String, coeffects_of_scope: Coeffects) {
        if !coeffects_of_scope.get_static_coeffects().is_empty() {
            self.global_state
                .lambda_coeffects_of_scope
                .insert(key, coeffects_of_scope);
        }
    }

    /// Clear the variables, upon entering a lambda
    fn enter_lambda(&mut self) {
        self.capture_state.vars = Default::default();
        self.capture_state.this_ = false;
        self.capture_state.generics = Default::default();
    }

    fn set_namespace(&mut self, namespace: Arc<namespace_env::Env>) {
        self.namespace = namespace;
    }

    /// Add a variable to the captured variables
    fn add_var<'s>(&mut self, scope: &Scope<'_>, var: impl Into<Cow<'s, str>>) {
        let var = var.into();

        // Don't bother if it's $this, as this is captured implicitly
        if var == special_idents::THIS {
            self.capture_state.this_ = true;
        } else if scope.should_capture_var(&var) {
            // If it's bound as a parameter or definite assignment, don't add it
            // Also don't add the pipe variable
            self.capture_state.vars.insert(var.into_owned());
        }
    }

    fn add_generic(&mut self, scope: &mut Scope<'_>, var: &str) {
        let reified_var_position = |is_fun| {
            let is_reified_var =
                |param: &Tparam| param.reified != ReifyKind::Erased && param.name.1 == var;
            if is_fun {
                scope.fun_tparams().iter().position(is_reified_var)
            } else {
                scope.class_tparams().iter().position(is_reified_var)
            }
        };

        if let Some(i) = reified_var_position(true) {
            let var = string_utils::reified::captured_name(true, i);
            self.capture_state.generics.insert(var);
        } else if let Some(i) = reified_var_position(false) {
            let var = string_utils::reified::captured_name(false, i);
            self.capture_state.generics.insert(var);
        }
    }
}

fn compute_vars(
    params: &[FunParam],
    body: &impl aast_visitor::Node<AstParams<(), String>>,
) -> Result<IndexSet<String>> {
    decl_vars::vars_from_ast(params, &body, false).map_err(Error::unrecoverable)
}

fn get_parameter_names(params: &[FunParam]) -> IndexSet<String> {
    params.iter().map(|p| p.name.to_string()).collect()
}

fn strip_id(id: &Id) -> &str {
    string_utils::strip_global_ns(&id.1)
}

fn make_class_name(name: &ClassName) -> String {
    string_utils::mangle_xhp_id(strip_id(name).to_string())
}

fn make_closure_name(scope: &Scope<'_>, state: &State) -> String {
    let per_fun_idx = scope.closure_cnt_per_fun;
    let name = scope.make_scope_name(&state.namespace);
    string_utils::closures::mangle_closure(&name, per_fun_idx)
}

fn make_closure(
    p: Pos,
    scope: &Scope<'_>,
    state: &State,
    ro_state: &'_ ReadOnlyState,
    lambda_vars: Vec<String>,
    fun_tparams: Vec<Tparam>,
    class_tparams: Vec<Tparam>,
    is_static: bool,
    mode: Mode,
    fd: &Fun_,
) -> Class_ {
    let md = Method_ {
        span: fd.span.clone(),
        annotation: fd.annotation,
        final_: false,
        abstract_: false,
        static_: is_static,
        readonly_this: fd.readonly_this.is_some(),
        visibility: Visibility::Public,
        name: Id(p.clone(), members::__INVOKE.into()),
        tparams: fun_tparams,
        where_constraints: vec![],
        params: fd.params.clone(),
        ctxs: fd.ctxs.clone(),
        unsafe_ctxs: None, // TODO(T70095684)
        body: fd.body.clone(),
        fun_kind: fd.fun_kind,
        user_attributes: fd.user_attributes.clone(),
        readonly_ret: fd.readonly_ret,
        ret: fd.ret.clone(),
        external: false,
        hidden: fd.hidden,
        doc_comment: fd.doc_comment.clone(),
    };

    let make_class_var = |name: &str| ClassVar {
        final_: false,
        xhp_attr: None,
        abstract_: false,
        readonly: false, // readonly on closure_convert
        visibility: Visibility::Private,
        type_: TypeHint((), None),
        id: Id(p.clone(), name.into()),
        expr: None,
        user_attributes: Default::default(),
        doc_comment: None,
        is_static: false,
        span: p.clone(),
    };

    let cvl = lambda_vars
        .iter()
        .map(|name| make_class_var(string_utils::locals::strip_dollar(name)));

    Class_ {
        span: p.clone(),
        annotation: fd.annotation,
        mode,
        user_attributes: Default::default(),
        file_attributes: vec![],
        final_: false,
        is_xhp: false,
        has_xhp_keyword: false,
        kind: ClassishKind::Cclass(Abstraction::Concrete),
        name: Id(p.clone(), make_closure_name(scope, state)),
        tparams: class_tparams,
        extends: vec![Hint(
            p.clone(),
            Box::new(Hint_::Happly(Id(p.clone(), "Closure".into()), vec![])),
        )],
        uses: vec![],
        xhp_attr_uses: vec![],
        xhp_category: None,
        reqs: vec![],
        implements: vec![],
        consts: vec![],
        typeconsts: vec![],
        vars: cvl.collect(),
        methods: vec![md],
        xhp_children: vec![],
        xhp_attrs: vec![],
        namespace: Arc::clone(&ro_state.empty_namespace),
        enum_: None,
        doc_comment: None,
        emit_id: Some(EmitId::Anonymous),
        // TODO(T116039119): Populate value with presence of internal attribute
        internal: false,
        // TODO: closures should have the visibility of the module they are defined in
        module: None,
        docs_url: None,
        package: None,
    }
}

/// Translate special identifiers `__CLASS__`, `__METHOD__` and `__FUNCTION__`
/// into literal strings. It's necessary to do this before closure conversion
/// because the enclosing class will be changed.
fn convert_id(outer_scope: &Scope<'_>, Id(p, s): Id) -> Expr_ {
    let ret = Expr_::mk_string;
    let name = |c: &ClassName| {
        Expr_::mk_string(string_utils::mangle_xhp_id(strip_id(c).to_string()).into())
    };

    let mut scope = outer_scope;
    while scope.summary.hidden() {
        match scope.parent {
            Some(p) => scope = p,
            None => break,
        }
    }

    match s {
        _ if s.eq_ignore_ascii_case(pseudo_consts::G__TRAIT__) => match scope.as_class_summary() {
            Some(cd) if cd.kind == ClassishKind::Ctrait => name(cd.name),
            _ => ret("".into()),
        },
        _ if s.eq_ignore_ascii_case(pseudo_consts::G__CLASS__) => match scope.as_class_summary() {
            Some(cd) if cd.kind != ClassishKind::Ctrait => name(cd.name),
            Some(_) => Expr_::mk_id(Id(p, s)),
            None => ret("".into()),
        },
        _ if s.eq_ignore_ascii_case(pseudo_consts::G__METHOD__) => {
            let (prefix, is_trait) = match scope.as_class_summary() {
                None => ("".into(), false),
                Some(cd) => (
                    string_utils::mangle_xhp_id(strip_id(cd.name).to_string()) + "::",
                    cd.kind == ClassishKind::Ctrait,
                ),
            };
            // for lambdas nested in trait methods HHVM replaces __METHOD__
            // with enclosing method name - do the same and bubble up from lambdas *
            let id_scope = if is_trait {
                scope.walk_scope().find(|x| {
                    let scope_is_in_lambda = match &x.summary {
                        ScopeSummary::Lambda(_) => true,
                        _ => false,
                    };
                    !scope_is_in_lambda
                })
            } else {
                scope.walk_scope().next()
            };

            match id_scope.map(|x| &x.summary) {
                Some(ScopeSummary::Function(fd)) => ret((prefix + strip_id(fd.name)).into()),
                Some(ScopeSummary::Method(md)) => ret((prefix + strip_id(md.name)).into()),
                Some(ScopeSummary::Lambda(_)) => ret((prefix + "{closure}").into()),
                // PHP weirdness: __METHOD__ inside a class outside a method returns class name
                Some(ScopeSummary::Class(cd)) => ret(strip_id(cd.name).into()),
                _ => ret("".into()),
            }
        }
        _ if s.eq_ignore_ascii_case(pseudo_consts::G__FUNCTION__) => match &scope.summary {
            ScopeSummary::Function(fd) => ret(strip_id(fd.name).into()),
            ScopeSummary::Method(md) => ret(strip_id(md.name).into()),
            ScopeSummary::Lambda(_) => ret("{closure}".into()),
            _ => ret("".into()),
        },
        _ if s.eq_ignore_ascii_case(pseudo_consts::G__LINE__) => {
            // If the expression goes on multi lines, we return the last line
            let (_, line, _, _) = p.info_pos_extended();
            Expr_::mk_int(line.to_string())
        }
        _ => Expr_::mk_id(Id(p, s)),
    }
}

fn make_class_info(c: &ClassSummary<'_>) -> ClosureEnclosingClassInfo {
    ClosureEnclosingClassInfo {
        kind: c.kind,
        name: c.name.1.clone(),
        parent_class_name: match c.extends {
            [x] => x.as_happly().map(|(id, _args)| id.1.clone()),
            _ => None,
        },
    }
}

pub fn make_fn_param(pos: Pos, lid: &LocalId, is_variadic: bool, is_inout: bool) -> FunParam {
    FunParam {
        annotation: (),
        type_hint: TypeHint((), None),
        pos: pos.clone(),
        name: local_id::get_name(lid).clone(),
        info: if is_variadic {
            FunParamInfo::ParamVariadic
        } else {
            FunParamInfo::ParamRequired
        },
        callconv: if is_inout {
            ParamKind::Pinout(pos)
        } else {
            ParamKind::Pnormal
        },
        readonly: None, // TODO
        splat: None,    // TODO
        // Synthetic parameters are always positional (named params preserved in make_closure)
        named: None,
        user_attributes: Default::default(),
        visibility: None,
    }
}

fn make_dyn_meth_caller_lambda(pos: &Pos, cexpr: &Expr, fexpr: &Expr, force: bool) -> Expr_ {
    let pos = || pos.clone();
    let obj_var = Box::new(Lid(pos(), local_id::make_unscoped("$o")));
    let meth_var = Box::new(Lid(pos(), local_id::make_unscoped("$m")));
    let obj_lvar = Expr((), pos(), Expr_::Lvar(obj_var.clone()));
    let meth_lvar = Expr((), pos(), Expr_::Lvar(meth_var.clone()));
    // AST for: return $o-><func>(...$args);
    let args_var = local_id::make_unscoped("$args");
    let variadic_param = make_fn_param(pos(), &args_var, true, false);
    let invoke_method = hack_expr!(
        pos = pos(),
        r#"#obj_lvar->#meth_lvar(...#{lvar(args_var)})"#
    );
    let attrs = if force {
        UserAttributes(vec![UserAttribute {
            name: Id(pos(), "__DynamicMethCallerForce".into()),
            params: vec![],
        }])
    } else {
        Default::default()
    };
    let ctxs = Some(Contexts(
        pos(),
        vec![Hint::new(
            pos(),
            Hint_::mk_happly(Id(pos(), string_utils::coeffects::CALLER.into()), vec![]),
        )],
    ));

    let fd = Fun_ {
        span: pos(),
        annotation: (),
        readonly_this: None, // TODO: readonly_this in closure_convert
        readonly_ret: None,  // TODO: readonly_ret in closure convert
        tparams: vec![],
        ret: TypeHint((), None),
        params: vec![
            make_fn_param(pos(), &obj_var.1, false, false),
            make_fn_param(pos(), &meth_var.1, false, false),
            variadic_param,
        ],
        ctxs,
        unsafe_ctxs: None,
        body: FuncBody {
            fb_ast: Block(vec![Stmt(
                pos(),
                Stmt_::Return(Box::new(Some(invoke_method))),
            )]),
        },
        fun_kind: FunKind::FSync,
        user_attributes: attrs,
        external: false,
        hidden: false,
        doc_comment: None,
    };
    let force_val = if force { Expr_::True } else { Expr_::False };
    let force_val_expr = Expr((), pos(), force_val);
    let efun = Expr(
        (),
        pos(),
        Expr_::mk_efun(Efun {
            fun: fd,
            use_: vec![],
            closure_class_name: None,
            is_expr_tree_virtual_expr: false,
        }),
    );
    let fun_handle = hack_expr!(
        pos = pos(),
        r#"\__SystemLib\dynamic_meth_caller(#{clone(cexpr)}, #{clone(fexpr)}, #efun, #force_val_expr)"#
    );
    fun_handle.2
}

fn add_reified_property(tparams: &[Tparam], vars: &mut Vec<ClassVar>) {
    if !tparams.iter().all(|t| t.reified == ReifyKind::Erased) {
        let p = Pos::NONE;
        // varray/vec that holds a list of type structures
        // this prop will be initilized during runtime
        let hint = Hint(
            p.clone(),
            Box::new(Hint_::Happly(Id(p.clone(), "\\HH\\varray".into()), vec![])),
        );
        vars.insert(
            0,
            ClassVar {
                final_: false,
                xhp_attr: None,
                doc_comment: None,
                abstract_: false,
                readonly: false,
                visibility: Visibility::Private,
                type_: TypeHint((), Some(hint)),
                id: Id(p.clone(), string_utils::reified::PROP_NAME.into()),
                expr: None,
                user_attributes: Default::default(),
                is_static: false,
                span: p,
            },
        )
    }
}

struct ClosureVisitor<'a, 'b> {
    state: Option<State>,
    ro_state: &'a ReadOnlyState,
    // We need 'b to be a real lifetime so that our `type Params` can refer to
    // it - but we don't actually have any fields that use it - so we need a
    // Phantom.
    phantom: std::marker::PhantomData<&'b ()>,
}

impl<'ast, 'a: 'b, 'b> VisitorMut<'ast> for ClosureVisitor<'a, 'b> {
    type Params = AstParams<Scope<'b>, Error>;

    fn object(&mut self) -> &mut dyn VisitorMut<'ast, Params = Self::Params> {
        self
    }

    fn visit_method_(&mut self, scope: &mut Scope<'b>, md: &mut Method_) -> Result<()> {
        let cd = scope.as_class_summary().ok_or_else(|| {
            Error::unrecoverable("unexpected scope shape - method is not inside the class")
        })?;
        let variables = Self::compute_variables_from_fun(&md.params, &md.body.fb_ast, None)?;
        let coeffects = Coeffects::from_ast(
            md.ctxs.as_ref(),
            &md.params,
            &md.tparams,
            scope.class_tparams(),
        );
        let si = ScopeSummary::Method(MethodSummary {
            coeffects,
            fun_kind: md.fun_kind,
            name: &md.name,
            span: &md.span,
            static_: md.static_,
            tparams: &md.tparams,
            hidden: md.hidden,
        });
        self.with_subscope(scope, si, variables, |self_, scope| {
            md.body.recurse(scope, self_)?;
            let uid = get_unique_id_for_method(&cd.name.1, &md.name.1);
            self_
                .state_mut()
                .record_function_state(uid, Coeffects::default());
            visit_mut(self_, scope, &mut md.params)?;
            Ok(())
        })?;
        Ok(())
    }

    fn visit_class_(&mut self, scope: &mut Scope<'b>, cd: &mut Class_) -> Result<()> {
        let variables = Variables::default();
        let si = ScopeSummary::Class(ClassSummary {
            extends: &cd.extends,
            kind: cd.kind,
            mode: cd.mode,
            name: &cd.name,
            span: &cd.span,
            tparams: &cd.tparams,
        });
        self.with_subscope(scope, si, variables, |self_, scope| -> Result<()> {
            visit_mut(self_, scope, &mut cd.methods)?;
            visit_mut(self_, scope, &mut cd.consts)?;
            visit_mut(self_, scope, &mut cd.vars)?;
            visit_mut(self_, scope, &mut cd.xhp_attrs)?;
            visit_mut(self_, scope, &mut cd.user_attributes)?;
            add_reified_property(&cd.tparams, &mut cd.vars);
            Ok(())
        })?;
        Ok(())
    }

    fn visit_def(&mut self, scope: &mut Scope<'b>, def: &mut Def) -> Result<()> {
        match def {
            // need to handle it ourselvses, because visit_fun_ is
            // called both for toplevel functions and lambdas
            Def::Fun(fd) => {
                let variables =
                    Self::compute_variables_from_fun(&fd.fun.params, &fd.fun.body.fb_ast, None)?;
                let coeffects =
                    Coeffects::from_ast(fd.fun.ctxs.as_ref(), &fd.fun.params, &fd.tparams, &[]);
                let si = ScopeSummary::Function(FunctionSummary {
                    coeffects,
                    fun_kind: fd.fun.fun_kind,
                    mode: fd.mode,
                    name: &fd.name,
                    span: &fd.fun.span,
                    tparams: &fd.tparams,
                    hidden: fd.fun.hidden,
                });
                self.with_subscope(scope, si, variables, |self_, scope| {
                    fd.fun.body.recurse(scope, self_)?;
                    let uid = get_unique_id_for_function(&fd.name.1);
                    self_
                        .state_mut()
                        .record_function_state(uid, Coeffects::default());
                    visit_mut(self_, scope, &mut fd.fun.params)?;
                    visit_mut(self_, scope, &mut fd.fun.user_attributes)?;
                    Ok(())
                })?;
                Ok(())
            }
            _ => def.recurse(scope, self),
        }
    }

    fn visit_class_id_(&mut self, scope: &mut Scope<'b>, cid: &mut ClassId_) -> Result<()> {
        if let ClassId_::CIreified(id) = cid {
            self.state_mut().add_generic(scope, &id.1);
        }
        cid.recurse(scope, self)
    }

    fn visit_hint_(&mut self, scope: &mut Scope<'b>, hint: &mut Hint_) -> Result<()> {
        if let Hint_::Happly(id, _) = hint {
            self.state_mut().add_generic(scope, id.name())
        };
        hint.recurse(scope, self)
    }

    fn visit_stmt_(&mut self, scope: &mut Scope<'b>, stmt: &mut Stmt_) -> Result<()> {
        match stmt {
            Stmt_::Awaitall(x) => {
                scope.check_if_in_async_context()?;
                x.recurse(scope, self)
            }
            Stmt_::Do(x) => {
                let (b1, LoopCond(_, b2, e)) = &mut **x;
                scope.with_in_using(false, |scope| visit_mut(self, scope, b1))?;
                scope.with_in_using(false, |scope| visit_mut(self, scope, b2))?;
                self.visit_expr(scope, e)
            }
            Stmt_::While(x) => {
                let (LoopCond(_, b1, e), b2) = &mut **x;
                self.visit_expr(scope, e)?;
                scope.with_in_using(false, |scope| visit_mut(self, scope, b1))?;
                scope.with_in_using(false, |scope| visit_mut(self, scope, b2))
            }
            Stmt_::Foreach(x) => {
                if x.1.is_await_as_v() || x.1.is_await_as_kv() {
                    scope.check_if_in_async_context()?
                }
                x.recurse(scope, self)
            }
            Stmt_::For(x) => {
                let (e1, e2, LoopIter(_, b3, e3), b) = &mut **x;

                for e in e1 {
                    self.visit_expr(scope, e)?;
                }
                if let Some(LoopCond(_, b2, e)) = e2 {
                    self.visit_expr(scope, e)?;
                    scope.with_in_using(false, |scope| visit_mut(self, scope, b2))?;
                }
                scope.with_in_using(false, |scope| visit_mut(self, scope, b))?;
                for e in e3 {
                    self.visit_expr(scope, e)?;
                }
                scope.with_in_using(false, |scope| visit_mut(self, scope, b3))
            }
            Stmt_::Switch(x) => {
                let (e, cl, dfl) = &mut **x;
                self.visit_expr(scope, e)?;
                scope.with_in_using(false, |scope| visit_mut(self, scope, cl))?;
                match dfl {
                    None => Ok(()),
                    Some(dfl) => scope.with_in_using(false, |scope| visit_mut(self, scope, dfl)),
                }
            }
            Stmt_::Using(x) => {
                if x.has_await {
                    scope.check_if_in_async_context()?;
                }
                for e in &mut x.exprs.1 {
                    self.visit_expr(scope, e)?;
                }
                scope.with_in_using(true, |scope| visit_mut(self, scope, &mut x.block))?;
                Ok(())
            }
            _ => stmt.recurse(scope, self),
        }
    }

    fn visit_expr(&mut self, scope: &mut Scope<'b>, Expr(_, pos, e): &mut Expr) -> Result<()> {
        stack_limit::maybe_grow(|| {
            let null = Expr_::mk_null();
            let e_owned = std::mem::replace(e, null);
            *e = match e_owned {
                Expr_::Efun(x) => self.convert_lambda(scope, x.fun, Some(x.use_))?,
                Expr_::Lfun(x) => self.convert_lambda(scope, x.0, None)?,
                Expr_::Lvar(id_orig) => {
                    let id = if self.ro_state.for_debugger_eval
                        && local_id::get_name(&id_orig.1) == special_idents::THIS
                        && scope.is_in_debugger_eval_fun()
                    {
                        Box::new(Lid(id_orig.0, (0, "$__debugger$this".to_string())))
                    } else {
                        id_orig
                    };
                    self.state_mut().add_var(scope, local_id::get_name(&id.1));
                    Expr_::Lvar(id)
                }
                Expr_::Id(id) if id.name().starts_with('$') => {
                    let state = self.state_mut();
                    state.add_var(scope, id.name());
                    state.add_generic(scope, id.name());
                    Expr_::Id(id)
                }
                Expr_::Id(id) => {
                    self.state_mut().add_generic(scope, id.name());
                    convert_id(scope, *id)
                }
                Expr_::Call(x) if is_dyn_meth_caller(&x) => {
                    self.visit_dyn_meth_caller(scope, x, &*pos)?
                }
                Expr_::Call(x) if is_meth_caller(&x) => self.visit_meth_caller(scope, x)?,
                Expr_::Call(x) if is_parent_call(&x) => {
                    self.state_mut().add_var(scope, "$this");
                    let mut res = Expr_::Call(x);
                    res.recurse(scope, self)?;
                    res
                }
                Expr_::ClassGet(mut x) => {
                    if let (id, PropOrMethod::IsMethod) = (&x.1, x.2) {
                        self.state_mut().add_var(scope, &id.1);
                    };
                    x.recurse(scope, self)?;
                    Expr_::ClassGet(x)
                }
                Expr_::Await(mut x) => {
                    scope.check_if_in_async_context()?;
                    x.recurse(scope, self)?;
                    Expr_::Await(x)
                }
                Expr_::Delay(mut x) => {
                    x.recurse(scope, self)?;
                    Expr_::Delay(x)
                }
                Expr_::ReadonlyExpr(mut x) => {
                    x.recurse(scope, self)?;
                    Expr_::ReadonlyExpr(x)
                }
                Expr_::ExpressionTree(mut x) => {
                    x.runtime_expr.recurse(scope, self)?;
                    Expr_::ExpressionTree(x)
                }
                Expr_::Pipe(mut x) => {
                    self.visit_expr(scope, &mut x.1)?;
                    scope.with_in_pipe(|scope| self.visit_expr(scope, &mut x.2))?;
                    Expr_::Pipe(x)
                }
                mut x => {
                    x.recurse(scope, self)?;
                    x
                }
            };
            Ok(())
        })
    }
}

impl<'a: 'b, 'b> ClosureVisitor<'a, 'b> {
    /// Calls a function in the scope of a sub-Scope as a child of `scope`.
    fn with_subscope<'s, F, R>(
        &mut self,
        scope: &'s Scope<'b>,
        si: ScopeSummary<'s>,
        variables: Variables,
        f: F,
    ) -> Result<(R, u32)>
    where
        'b: 's,
        F: FnOnce(&mut ClosureVisitor<'a, 's>, &mut Scope<'s>) -> Result<R>,
    {
        let mut scope = scope.new_child(si, variables)?;

        let mut self_ = ClosureVisitor {
            ro_state: self.ro_state,
            state: self.state.take(),
            phantom: Default::default(),
        };
        let res = f(&mut self_, &mut scope);
        self.state = self_.state;
        Ok((res?, scope.closure_cnt_per_fun))
    }

    fn state_mut(&mut self) -> &mut State {
        self.state.as_mut().unwrap()
    }

    #[inline(never)]
    fn visit_dyn_meth_caller(
        &mut self,
        scope: &mut Scope<'b>,
        mut x: Box<CallExpr>,
        pos: &Pos,
    ) -> Result<Expr_> {
        let force = if let Expr_::Id(ref id) = x.func.2 {
            strip_id(id) == "HH\\dynamic_meth_caller_force"
        } else {
            false
        };
        if let [carg, farg] = &mut *x.args {
            let cexpr = error::expect_normal_paramkind(carg)?;
            let fexpr = error::expect_normal_paramkind(farg)?;
            let mut res = make_dyn_meth_caller_lambda(pos, cexpr, fexpr, force);
            res.recurse(scope, self)?;
            Ok(res)
        } else {
            let mut res = Expr_::Call(x);
            res.recurse(scope, self)?;
            Ok(res)
        }
    }

    #[inline(never)]
    fn visit_meth_caller(&mut self, scope: &mut Scope<'b>, mut x: Box<CallExpr>) -> Result<Expr_> {
        if let [carg, farg] = &mut *x.args {
            let Expr(_, pc, cls) = error::expect_normal_paramkind_mut(carg)?;
            let Expr(_, pf, func) = error::expect_normal_paramkind(farg)?;
            match (&cls, func.as_string()) {
                (Expr_::ClassConst(cc), Some(_)) if string_utils::is_class(&(cc.1).1) => {
                    let mut cls_const = cls.as_class_const_mut();
                    let cid = match cls_const {
                        None => unreachable!(),
                        Some((ref mut cid, (_, _))) => cid,
                    };
                    match &cid.2 {
                        ClassId_::CIexpr(Expr(_, _, Expr_::Id(id))) if !is_selflike_keyword(id) => {
                            let mut res = Expr_::Call(x);
                            res.recurse(scope, self)?;
                            Ok(res)
                        }
                        ClassId_::CIreified(_) => {
                            let mut res = Expr_::Call(x);
                            res.recurse(scope, self)?;
                            Ok(res)
                        }
                        ClassId_::CIself | _ => Err(Error::fatal_parse(pc, "Invalid class")),
                    }
                }
                (_, Some(_)) => Err(Error::fatal_parse(pc, "Class must be a Class type")),
                (_, _) => Err(Error::fatal_parse(
                    pf,
                    "Method name must be a literal string",
                )),
            }
        } else {
            let mut res = Expr_::Call(x);
            res.recurse(scope, self)?;
            Ok(res)
        }
    }

    // Closure-convert a lambda expression, with use_vars_opt = Some vars
    // if there is an explicit `use` clause.
    fn convert_lambda(
        &mut self,
        scope: &mut Scope<'b>,
        mut fd: Fun_,
        use_vars_opt: Option<Vec<CaptureLid>>,
    ) -> Result<Expr_> {
        let is_long_lambda = use_vars_opt.is_some();
        let state = self.state_mut();

        // Remember the current capture and defined set across the lambda
        let capture_state = state.capture_state.clone();
        let coeffects_of_scope = scope
            .coeffects_of_scope()
            .map_or_else(Default::default, |co| co.clone());
        state.enter_lambda();
        if let Some(user_vars) = &use_vars_opt {
            for CaptureLid(_, Lid(p, id)) in user_vars.iter() {
                if local_id::get_name(id) == special_idents::THIS {
                    return Err(Error::fatal_parse(
                        p,
                        "Cannot use $this as lexical variable",
                    ));
                }
            }
        }

        let explicit_capture: Option<IndexSet<String>> = use_vars_opt.as_ref().map(|vars| {
            vars.iter()
                .map(|CaptureLid(_, Lid(_, (_, name)))| name.to_string())
                .collect()
        });
        let variables =
            Self::compute_variables_from_fun(&fd.params, &fd.body.fb_ast, explicit_capture)?;
        let coeffects = Coeffects::from_ast(fd.ctxs.as_ref(), &fd.params, &[], &[]);
        let si = ScopeSummary::Lambda(LambdaSummary {
            coeffects,
            explicit_capture: use_vars_opt.as_deref(),
            fun_kind: fd.fun_kind,
            span: &fd.span,
            hidden: fd.hidden,
        });
        let (_, closure_cnt_per_fun) =
            self.with_subscope(scope, si, variables, |self_, scope| {
                fd.body.recurse(scope, self_)?;
                for param in &mut fd.params {
                    visit_mut(self_, scope, &mut param.type_hint)?;
                }
                visit_mut(self_, scope, &mut fd.ret)?;
                Ok(())
            })?;

        scope.closure_cnt_per_fun = closure_cnt_per_fun + 1;

        let state = self.state.as_mut().unwrap();
        let current_generics = state.capture_state.generics.clone();

        // TODO(hrust): produce real unique local ids
        let fresh_lid = |name: String| CaptureLid((), Lid(Pos::NONE, (12345, name)));

        let lambda_vars: Vec<&String> = state
            .capture_state
            .vars
            .iter()
            .chain(current_generics.iter())
            // HHVM lists lambda vars in descending order - do the same
            .sorted()
            .rev()
            .collect();

        // Remove duplicates, (not efficient, but unlikely to be large),
        // remove variables that are actually just parameters
        let use_vars_opt: Option<Vec<CaptureLid>> = use_vars_opt.map(|use_vars| {
            let params = &fd.params;
            use_vars
                .into_iter()
                .rev()
                .unique_by(|lid| lid.1.name().to_string())
                .filter(|x| !params.iter().any(|y| x.1.name() == &y.name))
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
                .collect()
        });

        // For lambdas with explicit `use` variables, we ignore the computed
        // capture set and instead use the explicit set
        let (lambda_vars, use_vars): (Vec<String>, Vec<CaptureLid>) = match use_vars_opt {
            None => (
                lambda_vars.iter().map(|x| x.to_string()).collect(),
                lambda_vars
                    .iter()
                    .map(|x| fresh_lid(x.to_string()))
                    .collect(),
            ),
            Some(use_vars) => {
                // We still need to append the generics
                (
                    use_vars
                        .iter()
                        .map(|x| x.1.name())
                        .chain(current_generics.iter())
                        .map(|x| x.to_string())
                        .collect(),
                    use_vars
                        .iter()
                        .cloned()
                        .chain(current_generics.iter().map(|x| fresh_lid(x.to_string())))
                        .collect(),
                )
            }
        };

        let mut fun_tparams = scope.fun_tparams().to_vec(); // hiddden .clone()

        // Remove the type parmas from the lambda, convert all the HintTparams
        // to Tparams and add then to fun_tparams
        std::mem::take(&mut fd.tparams).into_iter().for_each(|x| {
            fun_tparams.push(Tparam {
                variance: Variance::Invariant,
                name: x.name,
                constraints: x.constraints,
                reified: ReifyKind::Erased,
                user_attributes: x
                    .user_attributes
                    .into_iter()
                    .map(|name| UserAttribute {
                        name,
                        params: vec![],
                    })
                    .collect(),
            })
        });
        let class_tparams = scope.class_tparams().to_vec(); // hiddden .clone()

        let is_static = if is_long_lambda {
            // long lambdas are never static
            false
        } else {
            // short lambdas can be made static if they don't capture this in
            // any form (including any nested lambdas)
            !state.capture_state.this_
        };

        // check if something can be promoted to static based on enclosing scope
        let is_static = is_static || scope.is_static();

        let pos = fd.span.clone();
        let lambda_vars_clone = lambda_vars.clone();
        let cd = make_closure(
            pos,
            scope,
            state,
            self.ro_state,
            lambda_vars,
            fun_tparams,
            class_tparams,
            is_static,
            scope.scope_fmode(),
            &fd,
        );

        let closure_class_name = cd.name.1.clone();
        if is_long_lambda {
            state
                .global_state
                .explicit_use_set
                .insert(closure_class_name.clone());
        }

        if let Some(cd) = scope.as_class_summary() {
            state
                .global_state
                .closure_enclosing_classes
                .insert(closure_class_name.clone(), make_class_info(cd));
        }

        // Restore capture and defined set
        // - adjust captured $this information if lambda that was just processed was
        //   converted into non-static one
        state.capture_state = CaptureState {
            this_: capture_state.this_ || !is_static,
            ..capture_state
        };
        state
            .global_state
            .closure_namespaces
            .insert(closure_class_name.clone(), state.namespace.clone());
        state.record_function_state(
            get_unique_id_for_method(&cd.name.1, &cd.methods.first().unwrap().name.1),
            coeffects_of_scope,
        );

        // Add lambda captured vars to current captured vars
        for var in lambda_vars_clone.into_iter() {
            state.add_var(scope, var)
        }
        for x in current_generics.iter() {
            state.capture_state.generics.insert(x.to_string());
        }

        state.closures.push(cd);

        let efun = Efun {
            fun: fd,
            use_: use_vars,
            closure_class_name: Some(closure_class_name),
            is_expr_tree_virtual_expr: false,
        };
        Ok(Expr_::mk_efun(efun))
    }

    fn compute_variables_from_fun(
        params: &[FunParam],
        body: &[Stmt],
        explicit_capture: Option<IndexSet<String>>,
    ) -> Result<Variables> {
        let parameter_names = get_parameter_names(params);
        let all_vars = compute_vars(params, &body)?;
        let explicit_capture = explicit_capture.unwrap_or_default();
        Ok(Variables {
            parameter_names,
            all_vars,
            explicit_capture,
        })
    }
}

fn is_dyn_meth_caller(x: &CallExpr) -> bool {
    if let Expr_::Id(ref id) = (x.func).2 {
        match strip_id(id) {
            "HH\\dynamic_meth_caller" | "HH\\dynamic_meth_caller_force" => true,
            _ => false,
        }
    } else {
        false
    }
}

fn is_meth_caller(x: &CallExpr) -> bool {
    if let Expr_::Id(ref id) = x.func.2 {
        match strip_id(id) {
            "HH\\meth_caller" | "meth_caller" => true,
            _ => false,
        }
    } else {
        false
    }
}

fn is_parent_call(x: &CallExpr) -> bool {
    match &x.func.2 {
        Expr_::ClassGet(deref!((ci, _, _))) | Expr_::ClassConst(deref!((ci, _))) => {
            matches!(&ci.2, ClassId_::CIparent)
        }
        _ => false,
    }
}

fn is_selflike_keyword(id: &Id) -> bool {
    string_utils::is_static(id)
}

fn hoist_toplevel_functions(defs: &mut Vec<Def>) {
    // Reorder the functions so that they appear first.
    let (funs, nonfuns): (Vec<Def>, Vec<Def>) = defs.drain(..).partition(|x| x.is_fun());
    defs.extend(funs);
    defs.extend(nonfuns);
}

fn prepare_defs(defs: &mut [Def]) -> usize {
    let mut class_count = 0;
    let mut typedef_count = 0;
    let mut const_count = 0;

    for def in defs.iter_mut() {
        match def {
            Def::Class(x) => {
                x.emit_id = Some(EmitId::EmitId(class_count));
                class_count += 1;
            }
            Def::Typedef(x) => {
                x.emit_id = Some(EmitId::EmitId(typedef_count));
                typedef_count += 1;
            }
            Def::Constant(x) => {
                x.emit_id = Some(EmitId::EmitId(const_count));
                const_count += 1;
            }
            Def::Namespace(_) => {
                // This should have already been flattened by rewrite_program.
                unreachable!()
            }
            Def::ClassAlias(_)
            | Def::FileAttributes(_)
            | Def::Fun(_)
            | Def::Module(_)
            | Def::SetModule(_)
            | Def::NamespaceUse(_)
            | Def::SetNamespaceEnv(_)
            | Def::Stmt(_) => {}
        }
    }

    class_count as usize
}

pub fn convert_toplevel_prog(
    e: &mut Emitter,
    defs: &mut Vec<Def>,
    namespace_env: Arc<namespace_env::Env>,
) -> Result<()> {
    prepare_defs(defs);

    let mut scope = Scope::toplevel(defs.as_slice())?;
    let ro_state = ReadOnlyState {
        empty_namespace: Arc::clone(&namespace_env),
        for_debugger_eval: e.for_debugger_eval,
    };
    let state = State::initial_state(namespace_env);

    let mut visitor = ClosureVisitor {
        state: Some(state),
        ro_state: &ro_state,
        phantom: Default::default(),
    };

    for def in defs.iter_mut() {
        visitor.visit_def(&mut scope, def)?;
        match def {
            Def::SetNamespaceEnv(x) => {
                visitor.state_mut().set_namespace(Arc::clone(&*x));
            }
            Def::Class(_)
            | Def::ClassAlias(_)
            | Def::Constant(_)
            | Def::FileAttributes(_)
            | Def::Fun(_)
            | Def::Module(_)
            | Def::SetModule(_)
            | Def::Namespace(_)
            | Def::NamespaceUse(_)
            | Def::Stmt(_)
            | Def::Typedef(_) => {}
        }
    }

    let mut state = visitor.state.take().unwrap();
    state.record_function_state(get_unique_id_for_main(), Coeffects::default());
    hoist_toplevel_functions(defs);
    let named_fun_defs = state.named_hoisted_functions.into_values().map(Def::mk_fun);
    defs.splice(0..0, named_fun_defs);
    for class in state.closures.into_iter() {
        defs.push(Def::mk_class(class));
    }
    *e.global_state_mut() = state.global_state;
    Ok(())
}
