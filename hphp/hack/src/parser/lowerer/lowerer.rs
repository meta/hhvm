// Copyright (c) 2019, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::cell::Ref;
use std::cell::RefCell;
use std::cell::RefMut;
use std::matches;
use std::rc::Rc;
use std::slice::Iter;
use std::str::FromStr;
use std::sync::Arc;
use std::sync::LazyLock;

use bstr::B;
use bstr::BString;
use bumpalo::Bump;
use escaper::*;
use hash::HashMap;
use hash::HashSet;
use hhbc_string_utils as string_utils;
use itertools::Either;
use itertools::Itertools;
use lint_rust::LintError;
use naming_special_names_rust as sn;
use naming_special_names_rust::classes as special_classes;
use naming_special_names_rust::literal;
use naming_special_names_rust::modules as special_modules;
use naming_special_names_rust::pre_namespaced_functions;
use naming_special_names_rust::special_idents;
use naming_special_names_rust::typehints as special_typehints;
use naming_special_names_rust::user_attributes as special_attrs;
use ocaml_helper::ParseIntError;
use ocaml_helper::parse_int;
use oxidized::aast;
use oxidized::aast::Binop;
use oxidized::aast_defs::ClassName;
use oxidized::aast_defs::ClassReq;
use oxidized::aast_defs::DocComment;
use oxidized::aast_visitor::AstParams;
use oxidized::aast_visitor::Node;
use oxidized::aast_visitor::Visitor;
use oxidized::ast;
use oxidized::ast::Expr;
use oxidized::ast::Expr_;
use oxidized::ast::PackageMembership;
use oxidized::ast_defs::Id;
use oxidized::diagnostics::Diagnostic as HHError;
use oxidized::diagnostics::Naming;
use oxidized::diagnostics::NastCheck;
use oxidized::file_info;
use oxidized::namespace_env::Env as NamespaceEnv;
use oxidized::namespace_env::Mode;
use oxidized::parser_options::ParserOptions;
use oxidized::pos::Pos;
use parser_core_types::indexed_source_text::IndexedSourceText;
use parser_core_types::lexable_token::LexablePositionedToken;
use parser_core_types::lexable_token::LexableToken;
use parser_core_types::source_text::SourceText;
use parser_core_types::syntax::SyntaxValueWithKind;
use parser_core_types::syntax_by_ref::positioned_token::PositionedToken;
use parser_core_types::syntax_by_ref::positioned_token::TokenFactory as PositionedTokenFactory;
use parser_core_types::syntax_by_ref::positioned_value::PositionedValue;
use parser_core_types::syntax_by_ref::syntax::Syntax;
use parser_core_types::syntax_by_ref::syntax_variant_generated::SyntaxVariant::*;
use parser_core_types::syntax_by_ref::syntax_variant_generated::*;
use parser_core_types::syntax_error;
use parser_core_types::syntax_kind;
use parser_core_types::syntax_trait::SyntaxTrait;
use parser_core_types::token_factory::TokenMutator;
use parser_core_types::token_kind::TokenKind as TK;
use regex::bytes::Regex;
use thiserror::Error;

use crate::desugar_expression_tree::desugar;
use crate::modifier;

fn unescape_single(s: &str) -> Result<BString, escaper::InvalidString> {
    Ok(escaper::unescape_single(s)?.into())
}

fn unescape_nowdoc(s: &str) -> Result<BString, escaper::InvalidString> {
    Ok(escaper::unescape_nowdoc(s)?.into())
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum ExprLocation {
    TopLevel,
    MemberSelect,
    InDoubleQuotedString,
    AsStatement,
    RightOfAssignment,
    RightOfAssignmentInUsingStatement,
    RightOfReturn,
    UsingStatement,
    CallReceiver,
    /// Lvalue context: LHS of `=`, foreach value/key.
    ///
    /// We initially parse shape and tuple destructuring as if they were
    /// expressions, then disambiguate here in the lowerer. This avoids
    /// arbitrary lookahead in the parser. For example, consider:
    ///
    ///     shape('x' => $x, 'y' => $y)  .... remainder of statement
    ///
    /// We can't distinguish between an expression statement and a
    /// destructuring assignment. Two possible completions:
    ///
    ///     shape('x' => $x, 'y' => $y);           // expression statement
    ///     shape('x' => $x, 'y' => $y) = $point;  // destructuring assignment
    ///
    /// We always parse as an expression, then in the lowerer produce distinct
    /// AAST nodes: `Expr_::Shape`/`Expr_::Tuple` for expressions vs
    /// `Expr_::DestructureShape`/`Expr_::DestructureTuple` for destructuring
    /// patterns.
    ///
    /// This two-pass technique is similar to legacy `list()` destructuring,
    /// except `list()` reuses a single `Expr_::List` node for both contexts
    /// and relies on the type checker and emitter to track where it appears.
    /// The dedicated AAST nodes are more explicit and less error-prone.
    LvaluePosition,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum SuspensionKind {
    SKSync,
    SKAsync,
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub enum TokenOp {
    Skip,
    Noop,
    LeftTrim(usize),
    RightTrim(usize),
}

#[derive(Debug)]
pub struct FunHdr {
    suspension_kind: SuspensionKind,
    readonly_this: Option<ast::ReadonlyKind>,
    name: ast::Sid,
    constrs: Vec<ast::WhereConstraintHint>,
    type_parameters: Vec<ast::Tparam>,
    parameters: Vec<ast::FunParam>,
    contexts: Option<ast::Contexts>,
    unsafe_contexts: Option<ast::Contexts>,
    readonly_return: Option<ast::ReadonlyKind>,
    return_type: Option<ast::Hint>,
    internal: bool,
}

impl FunHdr {
    fn make_empty(env: &Env<'_>) -> Self {
        Self {
            suspension_kind: SuspensionKind::SKSync,
            readonly_this: None,
            name: ast::Id(env.mk_none_pos(), String::from("<ANONYMOUS>")),
            constrs: vec![],
            type_parameters: vec![],
            parameters: vec![],
            contexts: None,
            unsafe_contexts: None,
            readonly_return: None,
            return_type: None,
            internal: false,
        }
    }
}

#[derive(Debug)]
pub struct State {
    // bool represents reification
    pub cls_generics: HashMap<String, bool>,
    // fn_generics also used for methods; maps are separate due to shadowing
    pub fn_generics: HashMap<String, bool>,
    pub in_static_method: bool,
    pub parent_maybe_reified: bool,
    /// Parsing errors emitted during lowering. Note that most parsing
    /// errors are emitted in the initial FFP parse.
    pub parsing_errors: Vec<(Pos, String)>,
    /// hh_errors captures errors after parsing, naming, nast, etc.
    pub hh_errors: Vec<HHError>,
    pub lint_errors: Vec<LintError>,
    pub doc_comments: Vec<Option<DocComment>>,

    pub local_id_counter: isize,

    // TODO(hrust): this check is to avoid crash in Ocaml.
    // Remove it after all Ocaml callers are eliminated.
    pub exp_recursion_depth: usize,
}

const EXP_RECURSION_LIMIT: usize = 30_000;

#[derive(Clone)]
enum PackageMembershipCache {
    Uncomputed,
    NoPackage,
    Package(PackageMembership),
}

#[derive(Clone)]
pub struct Env<'a> {
    pub codegen: oxidized::namespace_env::Mode,
    quick_mode: bool,
    /// Show errors even in quick mode.
    /// Hotfix until we can properly set up saved states to surface parse errors during
    /// typechecking properly.
    pub show_all_errors: bool,
    file_mode: file_info::Mode,
    pub top_level_statements: bool, /* Whether we are (still) considering TLSs*/

    pub active_experimental_features: HashSet<oxidized::experimental_features::FeatureName>,

    // Cache none pos, lazy_static doesn't allow Rc.
    pos_none: Pos,
    pub empty_ns_env: Arc<NamespaceEnv>,

    // Whether we have encountered an yield expression
    found_yield: bool,
    // Whether we have encountered an await expression
    found_await: bool,
    // Whether we are in the body of an async function
    pub in_async: bool,
    // Whether we are in an expression tree
    in_expr_tree: bool,
    // The position of the DSL Class of the enclosing expression tree, if any
    pub enclosing_et_pos: Option<Pos>,

    pub indexed_source_text: &'a IndexedSourceText<'a>,
    pub parser_options: &'a ParserOptions,

    pub token_factory: PositionedTokenFactory<'a>,
    pub arena: &'a Bump,

    expression_tree_class: Option<ClassName>,

    state: Rc<RefCell<State>>,

    /// Memoized package membership for this file.
    package: PackageMembershipCache,
}

impl<'a> Env<'a> {
    pub fn make(
        codegen: oxidized::namespace_env::Mode,
        quick_mode: bool,
        show_all_errors: bool,
        mode: file_info::Mode,
        active_experimental_features: HashSet<oxidized::experimental_features::FeatureName>,
        indexed_source_text: &'a IndexedSourceText<'a>,
        parser_options: &'a ParserOptions,
        namespace_env: Arc<NamespaceEnv>,
        token_factory: PositionedTokenFactory<'a>,
        arena: &'a Bump,
    ) -> Self {
        Env {
            codegen,
            quick_mode,
            show_all_errors,
            file_mode: mode,
            top_level_statements: true,
            active_experimental_features,
            found_yield: false,
            found_await: false,
            in_async: false,
            in_expr_tree: false,
            enclosing_et_pos: None,
            indexed_source_text,
            parser_options,
            pos_none: Pos::NONE,
            empty_ns_env: namespace_env,
            token_factory,
            arena,
            expression_tree_class: None,
            package: PackageMembershipCache::Uncomputed,

            state: Rc::new(RefCell::new(State {
                cls_generics: HashMap::default(),
                fn_generics: HashMap::default(),
                in_static_method: false,
                parent_maybe_reified: false,
                parsing_errors: vec![],
                doc_comments: vec![],
                local_id_counter: 1,
                hh_errors: vec![],
                lint_errors: vec![],
                exp_recursion_depth: 0,
            })),
        }
    }

    fn file_mode(&self) -> file_info::Mode {
        self.file_mode
    }

    fn should_surface_error(&self) -> bool {
        !self.quick_mode || self.show_all_errors
    }

    pub fn is_typechecker(&self) -> bool {
        matches!(self.codegen, Mode::ForTypecheck)
    }

    fn is_codegen(&self) -> bool {
        matches!(self.codegen, Mode::ForCodegen)
    }

    fn source_text(&self) -> &SourceText<'a> {
        self.indexed_source_text.source_text()
    }

    fn cls_generics_mut(&mut self) -> RefMut<'_, HashMap<String, bool>> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.cls_generics)
    }

    fn fn_generics_mut(&mut self) -> RefMut<'_, HashMap<String, bool>> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.fn_generics)
    }

    pub fn clear_generics(&mut self) {
        let mut s = self.state.borrow_mut();
        s.cls_generics = HashMap::default();
        s.fn_generics = HashMap::default();
    }

    // avoids returning a reference to the env
    pub fn get_reification(&self, id: &str) -> Option<bool> {
        let s = self.state.borrow();
        if let Some(reif) = s.fn_generics.get(id) {
            Some(*reif)
        } else {
            s.cls_generics.get(id).copied()
        }
    }

    fn in_static_method(&mut self) -> RefMut<'_, bool> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.in_static_method)
    }

    fn parent_maybe_reified(&mut self) -> RefMut<'_, bool> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.parent_maybe_reified)
    }

    pub fn parsing_errors(&mut self) -> RefMut<'_, Vec<(Pos, String)>> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.parsing_errors)
    }

    pub fn hh_errors(&mut self) -> RefMut<'_, Vec<HHError>> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.hh_errors)
    }

    pub fn lint_errors(&mut self) -> RefMut<'_, Vec<LintError>> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.lint_errors)
    }

    fn top_docblock(&self) -> Ref<'_, Option<DocComment>> {
        Ref::map(self.state.borrow(), |s| {
            s.doc_comments.last().unwrap_or(&None)
        })
    }

    fn exp_recursion_depth(&self) -> RefMut<'_, usize> {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.exp_recursion_depth)
    }

    fn next_local_id(&self) -> isize {
        let mut id = RefMut::map(self.state.borrow_mut(), |s| &mut s.local_id_counter);
        *id += 1;
        *id
    }

    fn push_docblock(&mut self, doc_comment: Option<DocComment>) {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.doc_comments).push(doc_comment)
    }

    fn pop_docblock(&mut self) {
        RefMut::map(self.state.borrow_mut(), |s| &mut s.doc_comments).pop();
    }

    fn mk_none_pos(&self) -> Pos {
        self.pos_none.clone()
    }

    fn clone_and_unset_toplevel_if_toplevel<'b, 'c>(
        e: &'b mut Env<'c>,
    ) -> impl AsMut<Env<'c>> + use<'c, 'b> {
        if e.top_level_statements {
            let mut cloned = e.clone();
            cloned.top_level_statements = false;
            Either::Left(cloned)
        } else {
            Either::Right(e)
        }
    }

    // A function body introduces a new scope for yields and awaits, and it
    // doesn't inherit whether it's async from its context, so we need to clear
    // these flags for checking the body, and then restore them after.
    fn reset_for_function_body<F, R>(
        &mut self,
        node: S<'a>,
        async_: SuspensionKind,
        p: F,
    ) -> Result<(R, bool)>
    where
        F: FnOnce(S<'a>, &mut Env<'a>) -> Result<R>,
    {
        if self.in_expr_tree {
            // Inside of an expression tree, we are going to lower a client
            // lambda, which shouldn't disturb our tracking of hack-level
            // await/yield/async
            Ok((p(node, self)?, false))
        } else {
            let outer_found_yield = self.found_yield;
            let outer_found_await = self.found_await;
            let outer_in_async = self.in_async;
            self.found_yield = false;
            self.found_await = false;
            self.in_async = matches!(async_, SuspensionKind::SKAsync);
            let r = p(node, self);
            let found_yield = self.found_yield;
            self.found_yield = outer_found_yield;
            self.found_await = outer_found_await;
            self.in_async = outer_in_async;
            Ok((r?, found_yield))
        }
    }
}

impl<'a> AsMut<Env<'a>> for Env<'a> {
    fn as_mut(&mut self) -> &mut Env<'a> {
        self
    }
}

type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug, Error, PartialEq)]
pub enum Error {
    #[error(
        "missing case in {expecting:?}.\n - pos: {pos:?}\n - unexpected: '{node_name:?}'\n - kind: {kind:?}\n"
    )]
    MissingSyntax {
        expecting: String,
        pos: Pos,
        node_name: String,
        kind: syntax_kind::SyntaxKind,
    },
    #[error("{message}")]
    ParsingError { message: String, pos: Pos },
}

fn emit_error<'a>(error: Error, env: &mut Env<'a>) {
    // Don't emit multiple parsing errors during lowering. Once we've
    // seen one parsing error, later parsing errors are rarely
    // meaningful.
    if !env.parsing_errors().is_empty() {
        return;
    }

    match error {
        Error::MissingSyntax {
            expecting,
            pos,
            node_name,
            ..
        } => {
            let msg = syntax_error::lowering_parsing_error(&node_name, &expecting);
            env.parsing_errors().push((pos, msg.to_string()));
        }
        Error::ParsingError { message, pos } => {
            env.parsing_errors().push((pos, message));
        }
    }
}

type S<'arena> = &'arena Syntax<'arena, PositionedToken<'arena>, PositionedValue<'arena>>;

fn p_pos<'a>(node: S<'a>, env: &Env<'_>) -> Pos {
    node.position_exclusive(env.indexed_source_text)
        .map_or_else(|| env.mk_none_pos(), Into::into)
}

fn raise_parsing_error<'a>(node: S<'a>, env: &mut Env<'a>, msg: &str) {
    let pos = p_pos(node, env);
    raise_parsing_error_(pos, env, msg)
}

fn raise_parsing_error_pos(pos: &Pos, env: &mut Env<'_>, msg: &str) {
    raise_parsing_error_(pos.clone(), env, msg)
}

fn raise_parsing_error_(pos: Pos, env: &mut Env<'_>, msg: &str) {
    if env.should_surface_error() || env.is_codegen() {
        env.parsing_errors().push((pos, String::from(msg)))
    }
}

fn raise_hh_error(env: &mut Env<'_>, err: HHError) {
    env.hh_errors().push(err);
}

fn raise_lint_error(env: &mut Env<'_>, err: LintError) {
    env.lint_errors().push(err);
}

fn parsing_error<N>(msg: impl Into<String>, pos: Pos) -> Result<N> {
    Err(Error::ParsingError {
        message: msg.into(),
        pos,
    })
}

fn text<'a>(node: S<'a>, env: &Env<'_>) -> String {
    String::from(node.text(env.source_text()))
}

fn text_str<'b, 'a>(node: S<'a>, env: &'b Env<'_>) -> &'b str {
    node.text(env.source_text())
}

fn lowering_error(env: &mut Env<'_>, pos: &Pos, text: &str, syntax_kind: &str) {
    if env.is_typechecker() && env.parsing_errors().is_empty() {
        raise_parsing_error_pos(
            pos,
            env,
            &syntax_error::lowering_parsing_error(text, syntax_kind),
        )
    }
}

fn raise_missing_syntax(expecting: &str, node: S<'_>, env: &mut Env<'_>) {
    let pos = p_pos(node, env);
    let text = text(node, env);
    lowering_error(env, &pos, &text, expecting);
}

fn missing_syntax<'a, N>(expecting: &str, node: S<'a>, env: &mut Env<'a>) -> Result<N> {
    let text = text(node, env);
    Err(Error::MissingSyntax {
        expecting: String::from(expecting),
        pos: p_pos(node, env),
        node_name: text,
        kind: node.kind(),
    })
}

fn map_optional<'a, F, R>(node: S<'a>, env: &mut Env<'a>, p: F) -> Result<Option<R>>
where
    F: FnOnce(S<'a>, &mut Env<'a>) -> Result<R>,
{
    match &node.children {
        Missing => Ok(None),
        _ => p(node, env).map(Some),
    }
}

fn map_optional_emit_error<'a, F, R>(node: S<'a>, env: &mut Env<'a>, p: F) -> Option<R>
where
    F: FnOnce(S<'a>, &mut Env<'a>) -> Result<R>,
{
    match &node.children {
        Missing => None,
        _ => match p(node, env) {
            Ok(v) => Some(v),
            Err(e) => {
                emit_error(e, env);
                None
            }
        },
    }
}

fn pos_module_name<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Sid> {
    if let ModuleName(c) = &node.children {
        if let SyntaxList(l) = &c.parts.children {
            let p = p_pos(node, env);
            let mut s = String::with_capacity(node.width());
            for i in l.iter() {
                match &i.children {
                    ListItem(li) => {
                        s += text_str(&li.item, env);
                        s += text_str(&li.separator, env);
                    }
                    _ => s += text_str(i, env),
                }
            }
            return Ok(ast::Id(p, s));
        }
    }
    missing_syntax("module name", node, env)
}

fn pos_qualified_name<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Sid> {
    if let QualifiedName(c) = &node.children {
        if let SyntaxList(l) = &c.parts.children {
            let p = p_pos(node, env);
            let mut s = String::with_capacity(node.width());
            for i in l.iter() {
                match &i.children {
                    ListItem(li) => {
                        s += text_str(&li.item, env);
                        s += text_str(&li.separator, env);
                    }
                    _ => s += text_str(i, env),
                }
            }
            return Ok(ast::Id(p, s));
        }
    }
    missing_syntax("qualified name", node, env)
}

fn pos_name<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Sid> {
    pos_name_(node, env, None)
}

fn lid_from_pos_name<'a>(pos: Pos, name: S<'a>, env: &mut Env<'a>) -> Result<ast::Lid> {
    let name = pos_name(name, env)?;
    Ok(ast::Lid::new(pos, name.1))
}

fn lid_from_name<'a>(name: S<'a>, env: &mut Env<'a>) -> Result<ast::Lid> {
    let name = pos_name(name, env)?;
    Ok(ast::Lid::new(name.0, name.1))
}

fn p_pstring<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Pstring> {
    p_pstring_(node, env, None)
}

fn p_pstring_<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
    drop_prefix: Option<char>,
) -> Result<ast::Pstring> {
    let ast::Id(p, id) = pos_name_(node, env, drop_prefix)?;
    Ok((p, id))
}

fn drop_prefix(s: &str, prefix: char) -> &str {
    if !s.is_empty() && s.starts_with(prefix) {
        &s[1..]
    } else {
        s
    }
}

fn pos_name_<'a>(node: S<'a>, env: &mut Env<'a>, drop_prefix_c: Option<char>) -> Result<ast::Sid> {
    match &node.children {
        QualifiedName(_) => pos_qualified_name(node, env),
        SimpleTypeSpecifier(c) => pos_name_(&c.specifier, env, drop_prefix_c),
        _ => {
            let mut name = node.text(env.indexed_source_text.source_text());
            if let Some(prefix) = drop_prefix_c {
                name = drop_prefix(name, prefix);
            }
            let p = p_pos(node, env);
            Ok(ast::Id(p, String::from(name)))
        }
    }
}

fn mk_str<'a, F>(node: S<'a>, env: &mut Env<'a>, mut content: &str, unescaper: F) -> BString
where
    F: Fn(&str) -> Result<BString, InvalidString>,
{
    if let Some('b') = content.chars().next() {
        content = content.get(1..).unwrap();
    }

    let len = content.len();
    let no_quotes_result = extract_unquoted_string(content, 0, len);
    match no_quotes_result {
        Ok(no_quotes) => {
            let result = unescaper(&no_quotes);
            match result {
                Ok(s) => s,
                Err(_) => {
                    raise_parsing_error(
                        node,
                        env,
                        &format!("Malformed string literal <<{}>>", &no_quotes),
                    );
                    BString::from("")
                }
            }
        }
        Err(_) => {
            raise_parsing_error(
                node,
                env,
                &format!("Malformed string literal <<{}>>", &content),
            );
            BString::from("")
        }
    }
}

fn unesc_dbl(s: &str) -> Result<BString, InvalidString> {
    let unesc_s = unescape_double(s)?;
    if unesc_s == B("''") || unesc_s == B("\"\"") {
        Ok(BString::from(""))
    } else {
        Ok(unesc_s)
    }
}

// TODO: return Cow<[u8]>
fn unesc_xhp(s: &[u8]) -> Vec<u8> {
    static WHITESPACE: LazyLock<Regex> = LazyLock::new(|| Regex::new("[\x20\t\n\r\x0c]+").unwrap());

    WHITESPACE.replace_all(s, &b" "[..]).into_owned()
}

fn unesc_xhp_attr(s: &[u8]) -> Vec<u8> {
    // TODO: change unesc_dbl to &[u8] -> BString
    let r = get_quoted_content(s);
    let r = unsafe { std::str::from_utf8_unchecked(r) };
    unesc_dbl(r).unwrap().into()
}

fn get_quoted_content(s: &[u8]) -> &[u8] {
    static QUOTED: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r#"^[\x20\t\n\r\x0c]*"((?:.|\n)*)""#).unwrap());

    QUOTED
        .captures(s)
        .and_then(|c| c.get(1))
        .map_or(s, |m| m.as_bytes())
}

fn token_kind<'a>(node: S<'a>) -> Option<TK> {
    match &node.children {
        Token(t) => Some(t.kind()),
        _ => None,
    }
}

fn parameter_modifier<'a>(node: S<'a>, kind: TK) -> Option<S<'a>> {
    let fields = match &node.children {
        ParameterDeclaration(c) => [&c.optional, &c.call_convention, &c.named, &c.readonly],
        ClosureParameterTypeSpecifier(c) => {
            [&c.optional, &c.call_convention, &c.named, &c.readonly]
        }
        _ => return None,
    };

    fields
        .into_iter()
        .find(|field| token_kind(field) == Some(kind))
}

fn check_valid_reified_hint<'a>(env: &mut Env<'a>, node: S<'a>, hint: &ast::Hint) {
    struct Checker<F: FnMut(&String)>(F);
    impl<'ast, F: FnMut(&String)> Visitor<'ast> for Checker<F> {
        type Params = AstParams<(), ()>;

        fn object(&mut self) -> &mut dyn Visitor<'ast, Params = Self::Params> {
            self
        }

        fn visit_hint(&mut self, c: &mut (), h: &ast::Hint) -> Result<(), ()> {
            match h.1.as_ref() {
                ast::Hint_::Happly(id, _) => {
                    self.0(&id.1);
                }
                ast::Hint_::Haccess(_, ids) => {
                    ids.iter().for_each(|id| self.0(&id.1));
                }
                _ => {}
            }
            h.recurse(c, self)
        }
    }

    if *env.in_static_method() {
        let f = |id: &String| {
            fail_if_invalid_reified_generic(node, env, id);
        };
        let mut visitor = Checker(f);
        visitor.visit_hint(&mut (), hint).unwrap();
    }
}

fn p_closure_parameter<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<(ast::Hint, Option<ast::HfParamInfo>)> {
    match &node.children {
        ClosureParameterTypeSpecifier(c) => {
            let optional = parameter_modifier(node, TK::Optional)
                .map(|modifier| p_optional(modifier, env))
                .transpose()?;
            let kind = parameter_modifier(node, TK::Inout)
                .map(|modifier| p_param_kind(modifier, env))
                .transpose()?
                .unwrap_or(ast::ParamKind::Pnormal);
            let readonlyness = parameter_modifier(node, TK::Readonly)
                .map(|modifier| p_readonly(modifier, env))
                .transpose()?;
            let splat = map_optional(&c.pre_ellipsis, env, p_splat)?;
            let named = if parameter_modifier(node, TK::Named).is_none() {
                None
            } else {
                Some(text(&c.name, env).to_string())
            };
            let info = Some(ast::HfParamInfo {
                kind,
                readonlyness,
                optional,
                splat,
                named,
            });
            let hint = p_hint(&c.type_, env)?;
            Ok((hint, info))
        }
        _ => missing_syntax("closure parameter", node, env),
    }
}

fn p_union_or_intersection_element<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint> {
    match &node.children {
        TupleOrUnionOrIntersectionElementTypeSpecifier(c) => {
            let hint = p_hint(&c.type_, env)?;
            Ok(hint)
        }
        _ => missing_syntax("union or intersection tuple element", node, env),
    }
}

fn map_shape_expression_field<'a, F, R>(
    node: S<'a>,
    env: &mut Env<'a>,
    f: F,
) -> Result<(ast::ShapeFieldName, R)>
where
    F: Fn(S<'a>, &mut Env<'a>) -> Result<R>,
{
    let inner = match &node.children {
        PrefixUnaryExpression(c) if token_kind(&c.operator) == Some(TK::Question) => {
            raise_parsing_error(
                node,
                env,
                "Optional field syntax `?` is only valid in shape destructuring patterns",
            );
            &c.operand
        }
        _ => node,
    };
    match &inner.children {
        FieldInitializer(c) => {
            let name = p_shape_field_name(&c.name, env)?;
            let value = f(&c.value, env)?;
            Ok((name, value))
        }
        VariableExpression(c) => {
            // Shape field punning: $foo becomes 'foo' => $foo
            let pos = p_pos(inner, env);
            let var_name = text_str(&c.expression, env);
            // Strip the '$' prefix to get the field name
            let field_name = var_name.strip_prefix('$').unwrap_or(var_name);
            let shape_field_name =
                ast::ShapeFieldName::SFlitStr((pos, field_name.to_string().into()));
            let value = f(inner, env)?;
            Ok((shape_field_name, value))
        }
        _ => missing_syntax("shape field", inner, env),
    }
}

fn p_shape_field_name<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ShapeFieldName> {
    use ast::ShapeFieldName::*;
    let is_valid_shape_literal = |t: &PositionedToken<'a>| {
        let is_str =
            t.kind() == TK::SingleQuotedStringLiteral || t.kind() == TK::DoubleQuotedStringLiteral;
        let text = t.text(env.source_text());
        let is_empty = text == "\'\'" || text == "\"\"";
        is_str && !is_empty
    };
    if let LiteralExpression(c) = &node.children {
        if let Token(t) = &c.expression.children {
            if is_valid_shape_literal(t) {
                let ast::Id(p, n) = pos_name(node, env)?;
                let unescp = if t.kind() == TK::SingleQuotedStringLiteral {
                    unescape_single
                } else {
                    unesc_dbl
                };
                let str_ = mk_str(node, env, &n, unescp);
                return Ok(SFlitStr((p, str_)));
            }
        }
    }
    match &node.children {
        NameofExpression(n) => {
            let cls_name = pos_name(&n.target, env)?;
            if string_utils::class_id_is_dynamic(&cls_name.1) {
                raise_parsing_error(
                    node,
                    env,
                    &syntax_error::invalid_lazy_class_shape_field(&cls_name.1),
                );
            };
            Ok(SFclassname(cls_name))
        }
        ScopeResolutionExpression(c) => {
            let cls_name = pos_name(&c.qualifier, env)?;
            if string_utils::is_parent(&cls_name.1) || string_utils::is_static(&cls_name.1) {
                raise_parsing_error(
                    node,
                    env,
                    &syntax_error::invalid_parent_or_static_shape_field_name,
                );
                return missing_syntax("shape field name", node, env);
            }
            let const_name = p_pstring(&c.name, env)?;
            if string_utils::is_class(&const_name.1) && env.is_typechecker() {
                raise_parsing_error(
                    node,
                    env,
                    &syntax_error::invalid_lazy_class_shape_field(&cls_name.1),
                );
            }
            Ok(SFclassConst(cls_name, const_name))
        }
        _ => {
            raise_parsing_error(node, env, &syntax_error::invalid_shape_field_name);
            let ast::Id(p, n) = pos_name(node, env)?;
            Ok(SFlitStr((p, mk_str(node, env, &n, unesc_dbl))))
        }
    }
}

fn p_shape_field<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ShapeElement> {
    match &node.children {
        FieldSpecifier(c) => {
            let optional = !c.question.is_missing();
            let name = p_shape_field_name(&c.name, env)?;
            let hint = p_hint(&c.type_, env)?;
            Ok(ast::ShapeElement::SEField(ast::ShapeFieldInfo {
                optional,
                hint,
                name,
            }))
        }
        AbsentFieldSpecifier(c) => {
            // `absent 'x'` desugars to an optional field of type `nothing`
            // (i.e. `?'x' => nothing`): the field is definitely not present.
            let name = p_shape_field_name(&c.name, env)?;
            let pos = p_pos(node, env);
            let hint = ast::Hint::new(
                pos.clone(),
                ast::Hint_::Happly(ast::Id(pos, special_typehints::NOTHING.to_owned()), vec![]),
            );
            Ok(ast::ShapeElement::SEField(ast::ShapeFieldInfo {
                optional: true,
                hint,
                name,
            }))
        }
        ShapeSplatSpecifier(c) => {
            let hint = p_hint(&c.type_, env)?;
            Ok(ast::ShapeElement::SESplat(hint))
        }
        _ => {
            let (name, hint) = map_shape_expression_field(node, env, p_hint)?;
            Ok(ast::ShapeElement::SEField(ast::ShapeFieldInfo {
                optional: false,
                name,
                hint,
            }))
        }
    }
}

fn p_targ<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Targ> {
    Ok(ast::Targ((), p_hint(node, env)?))
}

fn p_unary_hint<'a>(kw: S<'a>, ty: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint_> {
    Ok(ast::Hint_::Happly(
        pos_name(kw, env)?,
        could_map(ty, env, p_hint)?,
    ))
}

fn p_binary_hint<'a>(kw: S<'a>, key: S<'a>, ty: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint_> {
    let kw = pos_name(kw, env)?;
    let key = p_hint(key, env)?;
    let value = p_hint(ty, env)?;
    Ok(ast::Hint_::Happly(kw, vec![key, value]))
}

fn p_hint_<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint_> {
    use ast::Hint_::*;

    match &node.children {
        Token(token) if token.kind() == TK::Variable => {
            let ast::Id(_pos, name) = pos_name(node, env)?;
            Ok(Hvar(name))
        }
        /* Dirty hack; CastExpression can have type represented by token */
        Token(_) | SimpleTypeSpecifier(_) | QualifiedName(_) => {
            let ast::Id(pos, name) = pos_name(node, env)?;

            if "integer".eq_ignore_ascii_case(&name) {
                raise_hh_error(
                    env,
                    Naming::bad_builtin_type(pos.clone(), &name, special_typehints::INT),
                );
            } else if "boolean".eq_ignore_ascii_case(&name) {
                raise_hh_error(
                    env,
                    Naming::bad_builtin_type(pos.clone(), &name, special_typehints::BOOL),
                );
            } else if "double".eq_ignore_ascii_case(&name) || "real".eq_ignore_ascii_case(&name) {
                raise_hh_error(
                    env,
                    Naming::bad_builtin_type(pos.clone(), &name, special_typehints::FLOAT),
                );
            }

            if env.file_mode() != file_info::Mode::Mhhi && !env.is_codegen() {
                let sn = strip_ns(&name);
                if sn.starts_with(sn::coeffects::CONTEXTS)
                    || sn.starts_with(sn::coeffects::CAPABILITIES)
                {
                    raise_parsing_error(node, env, &syntax_error::direct_coeffects_reference);
                }
            }
            if name == "_" {
                Ok(Hwildcard)
            } else {
                Ok(Happly(ast::Id(pos, name), vec![]))
            }
        }
        ShapeTypeSpecifier(c) => {
            let allows_unknown_fields = !c.ellipsis.is_missing() || !c.ellipsis_type.is_missing();
            /* if last element lacks a separator and ellipsis is present, error */
            if allows_unknown_fields {
                if let SyntaxList(items) = &c.fields.children {
                    if let Some(ListItem(item)) = items.last().map(|i| &i.children) {
                        if item.separator.is_missing() {
                            raise_parsing_error(
                                node,
                                env,
                                &syntax_error::shape_type_ellipsis_without_trailing_comma,
                            )
                        }
                    }
                }
            }

            let field_map = could_map(&c.fields, env, p_shape_field)?;
            let mut set = HashSet::default();
            for elem in field_map.iter() {
                let f = match elem {
                    ast::ShapeElement::SEField(f) => f,
                    ast::ShapeElement::SESplat(_) => continue,
                };
                use ast::ShapeFieldName::*;
                use bstr::BStr;
                // This check is sketchy. It considers only the const names in class const case
                // i.e. shape(B::class => int, C::class => string) is not allowed because const
                // "class" has already been bound, ditto for B::X and C::X. There is a separate
                // check that errors when B::X and C::Y are from different classes.
                // TODO(T199272576) just combine with the Shape_name_check in OCaml
                let (field_pos, field_name): (&Pos, &BStr) = match &f.name {
                    SFclassname(Id(p, _name)) => (p, "class".into()), // TODO(T199272576) sketchy check, needs to be post-namespacing and model runtime names
                    SFclassConst(_, (p, name)) => (p, name.as_bytes().into()),
                    SFlitStr((p, name)) => (p, name.as_ref()),
                };

                if !set.insert(field_name) {
                    raise_hh_error(env, Naming::fd_name_already_bound(field_pos.clone()));
                }
            }

            let unknown_fields_type = if !c.ellipsis_type.is_missing() {
                Some(p_hint(&c.ellipsis_type, env)?)
            } else {
                None
            };

            Ok(Hshape(ast::NastShapeInfo {
                allows_unknown_fields,
                field_map,
                unknown_fields_type,
            }))
        }
        TupleTypeSpecifier(c) => {
            let nodes = c.types.syntax_node_to_list_skip_separator();
            let (min, _) = nodes.size_hint();
            let mut seen_optional = false;
            let mut required = Vec::with_capacity(min);
            let mut optional = Vec::with_capacity(min);
            let mut variadic = None;
            let mut splat = None;
            for n in nodes {
                match &n.children {
                    TupleOrUnionOrIntersectionElementTypeSpecifier(c) => {
                        let hint = p_hint(&c.type_, env)?;
                        let has_optional = !c.optional.is_missing();
                        let has_ellipsis = !c.ellipsis.is_missing();
                        let has_pre_ellipsis = !c.pre_ellipsis.is_missing();
                        if has_ellipsis && has_pre_ellipsis {
                            raise_parsing_error(
                                node,
                                env,
                                "A splat element cannot also be variadic",
                            );
                        }
                        if has_ellipsis || has_pre_ellipsis {
                            if variadic.is_some() || splat.is_some() {
                                raise_parsing_error(
                                    node,
                                    env,
                                    "There must be only one variadic or splat element",
                                );
                            } else if has_optional {
                                raise_parsing_error(
                                    node,
                                    env,
                                    &syntax_error::no_optional_on_variadic_parameter,
                                );
                            }
                            if has_ellipsis {
                                variadic = Some(hint);
                            } else if has_pre_ellipsis {
                                if seen_optional {
                                    raise_parsing_error(
                                        node,
                                        env,
                                        "There must be no splat elements following an optional element",
                                    );
                                }
                                splat = Some(hint);
                            }
                        } else {
                            if variadic.is_some() || splat.is_some() {
                                raise_parsing_error(
                                    node,
                                    env,
                                    "There must be no elements following a variadic or splat element",
                                );
                            }
                            if has_optional {
                                seen_optional = true;
                                optional.push(hint);
                            } else {
                                if seen_optional {
                                    raise_parsing_error(
                                        node,
                                        env,
                                        "There must be no required elements following an optional element",
                                    );
                                }
                                required.push(hint);
                            }
                        }
                    }
                    _ => raise_missing_syntax("tuple element", node, env),
                }
            }
            let extra = match splat {
                Some(h) => ast::TupleExtra::Hsplat(h),
                None => ast::TupleExtra::Hextra(ast::TupleExtraInfo { optional, variadic }),
            };
            Ok(Htuple(ast::TupleInfo { required, extra }))
        }
        UnionTypeSpecifier(c) => Ok(Hunion(could_map(
            &c.types,
            env,
            p_union_or_intersection_element,
        )?)),
        IntersectionTypeSpecifier(c) => Ok(Hintersection(could_map(
            &c.types,
            env,
            p_union_or_intersection_element,
        )?)),
        KeysetTypeSpecifier(c) => Ok(Happly(
            pos_name(&c.keyword, env)?,
            could_map(&c.type_, env, p_hint)?,
        )),
        VectorTypeSpecifier(c) => p_unary_hint(&c.keyword, &c.type_, env),
        ClassnameTypeSpecifier(c) => p_unary_hint(&c.keyword, &c.type_, env),
        TupleTypeExplicitSpecifier(c) => p_unary_hint(&c.keyword, &c.types, env),
        VarrayTypeSpecifier(c) => p_unary_hint(&c.keyword, &c.type_, env),
        DarrayTypeSpecifier(c) => p_binary_hint(&c.keyword, &c.key, &c.value, env),
        DictionaryTypeSpecifier(c) => p_unary_hint(&c.keyword, &c.members, env),
        GenericTypeSpecifier(c) => {
            let name = pos_name(&c.class_type, env)?;
            let args = &c.argument_list;
            let type_args = match &args.children {
                TypeArguments(c) => could_map(&c.types, env, p_hint)?,
                _ => missing_syntax("generic type arguments", args, env)?,
            };
            Ok(Happly(name, type_args))
        }
        ClassPtrTypeSpecifier(c) => {
            let cls = p_hint(&c.type_, env)?;
            let kind = match token_kind(&c.keyword) {
                Some(TK::Class) => ast::ClassPtrKind::CKclass,
                Some(TK::Enum) => ast::ClassPtrKind::CKenum,
                _ => missing_syntax("class pointer type", &c.keyword, env)?,
            };
            Ok(HclassPtr(kind, cls))
        }
        NullableTypeSpecifier(c) => Ok(Hoption(p_hint(&c.type_, env)?)),
        LikeTypeSpecifier(c) => Ok(Hlike(p_hint(&c.type_, env)?)),
        ClosureTypeSpecifier(c) => {
            let tparams = p_hint_tparam_l(&c.type_parameters, env)?;

            let (param_list, variadic_nodes): (Vec<S<'a>>, Vec<S<'a>>) = c
                .parameter_list
                .syntax_node_to_list_skip_separator()
                .partition(|n| match &n.children {
                    ClosureParameterTypeSpecifier(c) => c.ellipsis.is_missing(),
                    _ => true,
                });
            let (type_hints, info) = param_list
                .iter()
                .map(|p| p_closure_parameter(p, env))
                .collect::<Result<Vec<_>, _>>()?
                .into_iter()
                .unzip();
            // Partition variadic parameters into named and unnamed
            let mut unnamed_variadic_hints = Vec::new();
            let mut named_variadic_hints = Vec::new();
            for v in variadic_nodes.iter() {
                match &v.children {
                    ClosureParameterTypeSpecifier(c) => {
                        if !c.ellipsis.is_missing() && c.type_.is_missing() {
                            raise_parsing_error(v, env, "Cannot use ... without a typehint");
                        }
                        if parameter_modifier(v, TK::Optional).is_some() {
                            raise_parsing_error(
                                v,
                                env,
                                &syntax_error::no_optional_on_variadic_parameter,
                            );
                        }
                        let hint = Some(p_hint(&c.type_, env)?);
                        if parameter_modifier(v, TK::Named).is_none() {
                            unnamed_variadic_hints.push(hint);
                        } else {
                            named_variadic_hints.push(hint);
                        }
                    }
                    _ => panic!("expect variadic parameter"),
                }
            }
            if unnamed_variadic_hints.len() > 1 {
                return parsing_error(
                    format!(
                        "{} unnamed variadic parameters found. There should be no more than one.",
                        unnamed_variadic_hints.len()
                    ),
                    p_pos(&c.parameter_list, env),
                );
            }
            let variadic_ty = unnamed_variadic_hints.into_iter().next().unwrap_or(None);
            let named_variadic_ty = named_variadic_hints.into_iter().next().unwrap_or(None);
            let ctxs = p_contexts(
                &c.contexts,
                env,
                Some((
                    "A closure type hint cannot have a polymorphic context",
                    true,
                )),
            );
            Ok(Hfun(ast::HintFun {
                is_readonly: map_optional(&c.readonly_keyword, env, p_readonly)?,
                param_tys: type_hints,
                tparams,
                param_info: info,
                variadic_ty,
                named_variadic_ty,
                ctxs,
                return_ty: p_hint(&c.return_type, env)?,
                is_readonly_return: map_optional(&c.readonly_return, env, p_readonly)?,
            }))
        }
        AttributizedSpecifier(c) => {
            let attrs = p_user_attribute(&c.attribute_spec, env)?;
            let hint = p_hint(&c.type_, env)?;
            if attrs.iter().any(|attr| attr.name.1 != special_attrs::SOFT) {
                raise_parsing_error(node, env, &syntax_error::only_soft_allowed);
            }
            Ok(*soften_hint(&attrs, hint).1)
        }
        FunctionCtxTypeSpecifier(c) => {
            let ast::Id(_p, n) = pos_name(&c.variable, env)?;
            Ok(HfunContext(n))
        }
        TypeConstant(c) => {
            let child = pos_name(&c.right_type, env)?;
            match p_hint_(&c.left_type, env)? {
                Haccess(root, mut cs) => {
                    cs.push(child);
                    Ok(Haccess(root, cs))
                }
                Hvar(n) => {
                    let pos = p_pos(&c.left_type, env);
                    let root = ast::Hint::new(pos, Hvar(n));
                    Ok(Haccess(root, vec![child]))
                }
                Happly(ty, param) => {
                    if param.is_empty() {
                        let root = ast::Hint::new(ty.0.clone(), Happly(ty, param));
                        Ok(Haccess(root, vec![child]))
                    } else {
                        missing_syntax("type constant base", node, env)
                    }
                }
                _ => missing_syntax("type constant base", node, env),
            }
        }
        ReifiedTypeArgument(_) => {
            raise_parsing_error(node, env, &syntax_error::invalid_reified);
            missing_syntax("refied type", node, env)
        }
        TypeRefinement(c) => Ok(ast::Hint_::Hrefinement(
            p_hint(&c.type_, env)?,
            could_map(&c.members, env, p_refinement_member)?,
        )),
        _ => missing_syntax("type hint", node, env),
    }
}

fn p_hint<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint> {
    let hint_ = p_hint_(node, env)?;
    let pos = p_pos(node, env);
    let hint = ast::Hint::new(pos, hint_);
    check_valid_reified_hint(env, node, &hint);
    Ok(hint)
}

fn p_refinement_member<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Refinement> {
    match &node.children {
        TypeInRefinement(c) => Ok(ast::Refinement::Rtype(
            pos_name(&c.name, env)?,
            if c.type_.is_missing() {
                let (lower, upper) = p_tconstraints_into_lower_and_upper(&c.constraints, env);
                ast::TypeRefinement::TRloose(ast::TypeRefinementBounds { lower, upper })
            } else {
                ast::TypeRefinement::TRexact(p_hint(&c.type_, env)?)
            },
        )),
        CtxInRefinement(c) => {
            let name = pos_name(&c.name, env)?;
            if c.ctx_list.is_missing() {
                let (lower, upper) = p_ctx_constraints(&c.constraints, env)?;
                Ok(ast::Refinement::Rctx(
                    name,
                    ast::CtxRefinement::CRloose(ast::CtxRefinementBounds { lower, upper }),
                ))
            } else if let Some(hint) = p_context_list_to_intersection(
                &c.ctx_list,
                env,
                "Refinement members cannot alias polymorphic contexts",
            ) {
                Ok(ast::Refinement::Rctx(
                    name,
                    ast::CtxRefinement::CRexact(hint),
                ))
            } else {
                missing_syntax("refinement member's bound(s)", node, env)
            }
        }
        _ => missing_syntax("refinement member", node, env),
    }
}

fn p_simple_initializer<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Expr> {
    match &node.children {
        SimpleInitializer(c) => p_expr(&c.value, env),
        _ => missing_syntax("simple initializer", node, env),
    }
}

fn expand_type_args<'a>(ty: S<'a>, env: &mut Env<'a>) -> Result<Vec<ast::Hint>> {
    match &ty.children {
        TypeArguments(c) => could_map(&c.types, env, p_hint),
        _ => Ok(vec![]),
    }
}

fn p_afield<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Afield> {
    match &node.children {
        ElementInitializer(c) => Ok(ast::Afield::AFkvalue(
            p_expr(&c.key, env)?,
            p_expr(&c.value, env)?,
        )),
        _ => Ok(ast::Afield::AFvalue(p_expr(node, env)?)),
    }
}

fn p_field<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Field, Error> {
    match &node.children {
        ElementInitializer(c) => Ok(ast::Field(p_expr(&c.key, env)?, p_expr(&c.value, env)?)),
        _ => missing_syntax("key-value collection element", node, env),
    }
}

// We lower readonly lambda declarations as making the inner lambda have readonly_this.
fn process_readonly_expr(mut e: ast::Expr) -> Expr_ {
    match &mut e {
        ast::Expr(_, _, Expr_::Efun(efun)) if efun.fun.readonly_this.is_none() => {
            efun.fun.readonly_this = Some(ast::ReadonlyKind::Readonly);
            e.2
        }
        ast::Expr(_, _, Expr_::Lfun(l)) if l.0.readonly_this.is_none() => {
            l.0.readonly_this = Some(ast::ReadonlyKind::Readonly);
            e.2
        }
        _ => Expr_::mk_readonly_expr(e),
    }
}

fn check_intrinsic_type_arg_varity<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
    tys: Vec<ast::Hint>,
) -> Option<ast::CollectionTarg> {
    let count = tys.len();
    let mut tys = tys.into_iter();
    match count {
        2 => Some(ast::CollectionTarg::CollectionTKV(
            ast::Targ((), tys.next().unwrap()),
            ast::Targ((), tys.next().unwrap()),
        )),
        1 => Some(ast::CollectionTarg::CollectionTV(ast::Targ(
            (),
            tys.next().unwrap(),
        ))),
        0 => None,
        _ => {
            raise_parsing_error(node, env, &syntax_error::collection_intrinsic_many_typeargs);
            None
        }
    }
}

fn p_import_flavor<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ImportFlavor> {
    use ast::ImportFlavor::*;
    match token_kind(node) {
        Some(TK::Include) => Ok(Include),
        Some(TK::Require) => Ok(Require),
        Some(TK::Include_once) => Ok(IncludeOnce),
        Some(TK::Require_once) => Ok(RequireOnce),
        _ => missing_syntax("import flavor", node, env),
    }
}

fn p_null_flavor<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::OperatorNullFlavor> {
    use ast::OperatorNullFlavor::*;
    match token_kind(node) {
        Some(TK::QuestionMinusGreaterThan) => Ok(Nullsafe),
        Some(TK::MinusGreaterThan) => Ok(Regular),
        _ => missing_syntax("null flavor", node, env),
    }
}

fn wrap_unescaper<F>(s: &str, unescaper: F, err_pos: Pos) -> Result<BString>
where
    F: FnOnce(&str) -> Result<BString, InvalidString>,
{
    unescaper(s).map_err(|e| Error::ParsingError {
        message: e.msg,
        pos: err_pos,
    })
}

fn fail_if_invalid_class_creation<'a>(node: S<'a>, env: &mut Env<'a>, id: impl AsRef<str>) {
    let id = id.as_ref();
    let is_in_static_method = *env.in_static_method();
    if is_in_static_method
        && ((string_utils::is_self(id) && env.cls_generics_mut().values().any(|reif| *reif))
            || (string_utils::is_parent(id) && *env.parent_maybe_reified()))
    {
        raise_parsing_error(node, env, &syntax_error::static_method_reified_obj_creation);
    }
}

fn fail_if_invalid_reified_generic<'a>(node: S<'a>, env: &mut Env<'a>, id: impl AsRef<str>) {
    let is_in_static_method = *env.in_static_method();
    if is_in_static_method && *env.cls_generics_mut().get(id.as_ref()).unwrap_or(&false) {
        raise_parsing_error(
            node,
            env,
            &syntax_error::cls_reified_generic_in_static_method,
        );
    }
}

fn expr_to_class_id<'a>(env: &mut Env<'a>, pos: Pos, e: ast::Expr) -> Result<ast::ClassId> {
    let Expr(ex, epos, expr_) = e;
    match expr_ {
        Expr_::Id(name) => match env.get_reification(&name.1) {
            Some(true) => Ok(ast::ClassId((), pos, ast::ClassId_::CIreified(*name))),
            Some(false) => parsing_error(syntax_error::erased_generic_as_class_id, epos),
            None if string_utils::is_self(&name.1) => {
                Ok(ast::ClassId((), epos, ast::ClassId_::CIself))
            }
            None if string_utils::is_parent(&name.1) => {
                Ok(ast::ClassId((), epos, ast::ClassId_::CIparent))
            }
            None => Ok(ast::ClassId(
                (),
                pos,
                ast::ClassId_::CIexpr(Expr(ex, epos, Expr_::Id(name))),
            )),
        },
        expr_ => Ok(ast::ClassId(
            (),
            pos,
            ast::ClassId_::CIexpr(Expr(ex, epos, expr_)),
        )),
    }
}

fn rfind(s: &[u8], mut i: usize, c: u8) -> Option<usize> {
    if i >= s.len() {
        return None;
    }
    i += 1;
    while i > 0 {
        i -= 1;
        if s[i] == c {
            return Some(i);
        }
    }
    None
}

fn prep_string2<'a>(
    nodes: &'a [Syntax<'a, PositionedToken<'a>, PositionedValue<'a>>],
    env: &mut Env<'a>,
) -> Result<(TokenOp, TokenOp)> {
    use TokenOp::*;
    let is_quote = |c| c == b'\"' || c == b'`';
    let start_is_quote = |s: &[u8]| {
        (!s.is_empty() && is_quote(s[0])) || (s.len() > 1 && (s[0] == b'b' && s[1] == b'\"'))
    };
    let last_is_quote = |s: &[u8]| !s.is_empty() && is_quote(s[s.len() - 1]);
    let is_heredoc = |s: &[u8]| s.len() > 3 && &s[0..3] == b"<<<";
    let mut nodes = nodes.iter();
    let first = nodes.next();
    match first.map(|n| &n.children) {
        Some(Token(t)) => {
            let raise = |env| {
                raise_parsing_error(first.unwrap(), env, "Malformed String2 SyntaxList");
            };
            let text = t.text_raw(env.source_text());
            if start_is_quote(text) {
                let first_token_op = match text[0] {
                    b'b' if text.len() > 2 => LeftTrim(2),
                    _ if is_quote(text[0]) && text.len() > 1 => LeftTrim(1),
                    _ => Skip,
                };
                if let Some(Token(t)) = nodes.last().map(|n| &n.children) {
                    let last_text = t.text_raw(env.source_text());
                    if last_is_quote(last_text) {
                        let last_taken_op = match last_text.len() {
                            n if n > 1 => RightTrim(1),
                            _ => Skip,
                        };
                        return Ok((first_token_op, last_taken_op));
                    }
                }
                raise(env);
                Ok((first_token_op, Noop))
            } else if is_heredoc(text) {
                let trim_size =
                    text.iter()
                        .position(|c| *c == b'\n')
                        .ok_or_else(|| Error::ParsingError {
                            message: String::from("newline not found"),
                            pos: p_pos(first.unwrap(), env),
                        })?
                        + 1;
                let first_token_op = match trim_size {
                    _ if trim_size == text.len() => Skip,
                    _ => LeftTrim(trim_size),
                };
                if let Some(Token(t)) = nodes.last().map(|n| &n.children) {
                    let text = t.text_raw(env.source_text());
                    let len = text.len();
                    if len != 0 {
                        let n = (match rfind(text, len - 2, b'\n') {
                            Some(n) => Ok(n),
                            None => Err(Error::ParsingError {
                                message: String::from("newline not found"),
                                pos: p_pos(first.unwrap(), env),
                            }),
                        })?;
                        let last_token_op = match n {
                            0 => Skip,
                            _ => RightTrim(len - n),
                        };
                        return Ok((first_token_op, last_token_op));
                    }
                }
                raise(env);
                Ok((first_token_op, Noop))
            } else {
                Ok((Noop, Noop))
            }
        }
        _ => Ok((Noop, Noop)),
    }
}

fn process_token_op<'a>(env: &mut Env<'a>, op: TokenOp, node: S<'a>) -> Result<Option<S<'a>>> {
    use TokenOp::*;
    match op {
        LeftTrim(n) => match &node.children {
            Token(t) => {
                let token = env.token_factory.trim_left(t, n);
                let node = env.arena.alloc(Syntax::make_token(token));
                Ok(Some(node))
            }
            _ => missing_syntax("token in operator", node, env),
        },
        RightTrim(n) => match &node.children {
            Token(t) => {
                let token = env.token_factory.trim_right(t, n);
                let node = env.arena.alloc(Syntax::make_token(token));
                Ok(Some(node))
            }
            _ => missing_syntax("token in operator", node, env),
        },
        _ => Ok(None),
    }
}

fn p_string2<'a>(
    nodes: &'a [Syntax<'a, PositionedToken<'a>, PositionedValue<'a>>],
    env: &mut Env<'a>,
) -> Result<Vec<ast::Expr>> {
    use TokenOp::*;
    let (head_op, tail_op) = prep_string2(nodes, env)?;
    let mut result = Vec::with_capacity(nodes.len());
    let mut i = 0;
    let last = nodes.len() - 1;
    while i <= last {
        let op = match i {
            0 => head_op,
            _ if i == last => tail_op,
            _ => Noop,
        };

        if op == Skip {
            i += 1;
            continue;
        }

        let node = process_token_op(env, op, &nodes[i])?;
        let node = node.unwrap_or(&nodes[i]);

        if token_kind(node) == Some(TK::Dollar) && i < last {
            if let EmbeddedBracedExpression(_) = &nodes[i + 1].children {
                raise_parsing_error(&nodes[i + 1], env, &syntax_error::outside_dollar_str_interp);

                result.push(p_expr_with_loc(
                    ExprLocation::InDoubleQuotedString,
                    &nodes[i + 1],
                    env,
                    None,
                )?);
                i += 2;
                continue;
            }
        }

        result.push(p_expr_with_loc(
            ExprLocation::InDoubleQuotedString,
            node,
            env,
            None,
        )?);
        i += 1;
    }
    Ok(result)
}

fn p_expr<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Expr> {
    p_expr_with_loc(ExprLocation::TopLevel, node, env, None)
}

fn p_expr_for_function_call_arguments<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Argument> {
    match &node.children {
        DecoratedExpression(DecoratedExpressionChildren {
            decorator,
            expression,
        }) if token_kind(decorator) == Some(TK::Inout) => Ok(aast::Argument::Ainout(
            p_pos(decorator, env),
            p_expr(expression, env)?,
        )),
        NamedArgument(NamedArgumentChildren {
            name,
            equal: _,
            expression,
        }) => {
            let name = pos_name(name, env)?;
            let expr = p_expr(expression, env)?;
            Ok(aast::Argument::Anamed(name, expr))
        }
        _ => Ok(aast::Argument::Anormal(p_expr(node, env)?)),
    }
}

fn p_expr_for_normal_argument<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Argument> {
    Ok(ast::Argument::Anormal(p_expr(node, env)?))
}

fn p_expr_with_loc<'a>(
    location: ExprLocation,
    node: S<'a>,
    env: &mut Env<'a>,
    parent_pos: Option<Pos>,
) -> Result<ast::Expr> {
    // We use location=CallReceiver to set PropOrMethod::IsMethod on ObjGet
    // But only if it is the immediate node.
    let location = match (location, &node.children) {
        (
            ExprLocation::CallReceiver,
            MemberSelectionExpression(_)
            | SafeMemberSelectionExpression(_)
            | EmbeddedMemberSelectionExpression(_)
            | ScopeResolutionExpression(_),
        ) => location,
        (ExprLocation::CallReceiver, _) => ExprLocation::TopLevel,
        (_, _) => location,
    };
    match &node.children {
        BracedExpression(c) => {
            // Either a dynamic method lookup on a dynamic value:
            //   $foo->{$meth_name}();
            // or an XHP splice.
            //   <p id={$id}>hello</p>;
            // In both cases, unwrap, consistent with parentheses.
            p_expr_with_loc(location, &c.expression, env, parent_pos)
        }
        ParenthesizedExpression(c) => p_expr_with_loc(location, &c.expression, env, parent_pos),
        _ => {
            let pos = p_pos(node, env);
            let expr_ = p_expr_recurse(location, node, env, parent_pos)?;
            Ok(ast::Expr::new((), pos, expr_))
        }
    }
}

fn p_expr_lit<'a>(
    location: ExprLocation,
    _parent: S<'a>,
    expr: S<'a>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    match &expr.children {
        Token(_) => {
            let s = expr.text(env.indexed_source_text.source_text());
            let check_lint_err = |e: &mut Env<'a>, s: &str, expected: &str| {
                if !e.is_codegen() && s != expected {
                    raise_lint_error(e, LintError::lowercase_constant(p_pos(expr, e), s));
                }
            };
            match (location, token_kind(expr)) {
                (ExprLocation::InDoubleQuotedString, _) if env.is_codegen() => {
                    Ok(Expr_::String(mk_str(expr, env, s, unesc_dbl)))
                }
                (_, Some(TK::DecimalLiteral))
                | (_, Some(TK::OctalLiteral))
                | (_, Some(TK::HexadecimalLiteral))
                | (_, Some(TK::BinaryLiteral)) => {
                    match parse_int(s) {
                        Err(ParseIntError::OutOfRange) => {
                            raise_parsing_error(expr, env, &syntax_error::out_of_int_range(s));
                        }
                        Err(ParseIntError::InvalidDigit(int_kind)) => {
                            raise_parsing_error(
                                expr,
                                env,
                                &syntax_error::invalid_integer_digit(int_kind),
                            );
                            missing_syntax(&format!("{}", int_kind), expr, env)?;
                        }
                        Err(ParseIntError::Empty) => {
                            missing_syntax("int literal", expr, env)?;
                        }
                        Ok(_) => {}
                    }
                    Ok(Expr_::Int(s.into()))
                }
                (_, Some(TK::FloatingLiteral)) => {
                    // f64::from_str accepts more string than Hacklang, invalid Hack float literal
                    // is caught in lexer.
                    if f64::from_str(s).is_err() {
                        raise_parsing_error(expr, env, &syntax_error::out_of_float_range(s))
                    }
                    Ok(Expr_::Float(s.into()))
                }
                (_, Some(TK::SingleQuotedStringLiteral)) => {
                    Ok(Expr_::String(mk_str(expr, env, s, unescape_single)))
                }
                (_, Some(TK::DoubleQuotedStringLiteral)) => {
                    Ok(Expr_::String(mk_str(expr, env, s, unescape_double)))
                }
                (_, Some(TK::HeredocStringLiteral)) => {
                    Ok(Expr_::String(mk_str(expr, env, s, unescape_heredoc)))
                }
                (_, Some(TK::NowdocStringLiteral)) => {
                    Ok(Expr_::String(mk_str(expr, env, s, unescape_nowdoc)))
                }
                (_, Some(TK::NullLiteral)) => {
                    check_lint_err(env, s, literal::NULL);
                    Ok(Expr_::Null)
                }
                (_, Some(TK::BooleanLiteral)) => {
                    if s.eq_ignore_ascii_case(literal::FALSE) {
                        check_lint_err(env, s, literal::FALSE);
                        Ok(Expr_::False)
                    } else if s.eq_ignore_ascii_case(literal::TRUE) {
                        check_lint_err(env, s, literal::TRUE);
                        Ok(Expr_::True)
                    } else {
                        missing_syntax(&format!("boolean (not: {})", s), expr, env)
                    }
                }
                _ => missing_syntax("literal", expr, env),
            }
        }
        SyntaxList(ts) => Ok(Expr_::String2(p_string2(ts, env)?)),
        _ => missing_syntax("literal expressoin", expr, env),
    }
}

fn p_expr_recurse<'a>(
    location: ExprLocation,
    node: S<'a>,
    env: &mut Env<'a>,
    parent_pos: Option<Pos>,
) -> Result<Expr_> {
    if *env.exp_recursion_depth() >= EXP_RECURSION_LIMIT {
        Err(Error::ParsingError {
            message: "Expression recursion limit reached".into(),
            pos: parent_pos.unwrap_or_else(|| env.mk_none_pos()),
        })
    } else {
        *env.exp_recursion_depth() += 1;
        let r = stack_limit::maybe_grow(|| p_expr_impl(location, node, env, parent_pos));
        *env.exp_recursion_depth() -= 1;
        r
    }
}

fn split_args_unpacked_arg<'a>(
    arg_list_node: S<'a>,
    e: &mut Env<'a>,
) -> Result<(Vec<ast::Argument>, Option<ast::Expr>)> {
    let mut iter = arg_list_node.syntax_node_to_list_skip_separator();
    let (cap, _) = iter.size_hint();
    iter.try_fold(
        (Vec::with_capacity(cap), None),
        |(mut args, unpacked_arg), arg| {
            if let DecoratedExpression(c) = &arg.children
                && token_kind(&c.decorator) == Some(TK::DotDotDot)
            {
                if unpacked_arg.is_some() {
                    raise_parsing_error(arg, e, &syntax_error::error2084);
                    Ok((args, unpacked_arg))
                } else {
                    let parsed_splat = p_expr(&c.expression, e)?;
                    Ok((args, Some(parsed_splat)))
                }
            } else {
                let parsed_arg = p_expr_for_function_call_arguments(arg, e)?;
                args.push(parsed_arg);
                Ok((args, unpacked_arg))
            }
        },
    )
}

fn p_expr_impl<'a>(
    location: ExprLocation,
    node: S<'a>,
    env: &mut Env<'a>,
    parent_pos: Option<Pos>,
) -> Result<Expr_> {
    let pos = match parent_pos {
        Some(pos) => pos,
        None => p_pos(node, env),
    };
    match &node.children {
        LambdaExpression(c) => p_lambda_expression(c, env, pos),
        BracedExpression(c) => p_expr_recurse(location, &c.expression, env, None),
        EmbeddedBracedExpression(c) => p_expr_recurse(location, &c.expression, env, Some(pos)),
        ParenthesizedExpression(c) => p_expr_recurse(location, &c.expression, env, None),
        DictionaryIntrinsicExpression(c) => {
            let ty_args = expand_type_args(&c.explicit_type, env)?;
            let hints = if ty_args.len() == 2 {
                let mut tys = ty_args.into_iter();
                Some((
                    ast::Targ((), tys.next().unwrap()),
                    ast::Targ((), tys.next().unwrap()),
                ))
            } else if ty_args.is_empty() {
                None
            } else {
                raise_parsing_error(
                    &c.explicit_type,
                    env,
                    "`dict` takes exactly two type arguments",
                );
                None
            };

            Ok(Expr_::mk_key_val_collection(
                (p_pos(&c.keyword, env), aast::KvcKind::Dict),
                hints,
                could_map(&c.members, env, p_field)?,
            ))
        }
        KeysetIntrinsicExpression(c) => {
            let mut ty_args = expand_type_args(&c.explicit_type, env)?;
            let hint = if ty_args.len() == 1 {
                Some(ast::Targ((), ty_args.pop().unwrap()))
            } else if ty_args.is_empty() {
                None
            } else {
                raise_parsing_error(
                    &c.explicit_type,
                    env,
                    "`keyset` takes exactly one type argument",
                );
                None
            };

            Ok(Expr_::mk_val_collection(
                (p_pos(&c.keyword, env), aast::VcKind::Keyset),
                hint,
                could_map(&c.members, env, p_expr)?,
            ))
        }
        VectorIntrinsicExpression(c) => {
            let mut ty_args = expand_type_args(&c.explicit_type, env)?;
            let hint = if ty_args.len() == 1 {
                Some(ast::Targ((), ty_args.pop().unwrap()))
            } else if ty_args.is_empty() {
                None
            } else {
                raise_parsing_error(
                    &c.explicit_type,
                    env,
                    "`vec` takes exactly one type argument",
                );
                None
            };

            Ok(Expr_::mk_val_collection(
                (p_pos(&c.keyword, env), aast::VcKind::Vec),
                hint,
                could_map(&c.members, env, p_expr)?,
            ))
        }
        CollectionLiteralExpression(c) => p_collection_literal_expr(node, c, env),
        VarrayIntrinsicExpression(c) => p_varray_intrinsic_expr(node, c, env),
        DarrayIntrinsicExpression(c) => p_darray_intrinsic_expr(node, c, env),
        ListExpression(c) => p_list_expr(c, env),
        EvalExpression(c) => p_special_call(&c.keyword, &c.argument, env),
        IssetExpression(c) => p_special_call(&c.keyword, &c.argument_list, env),
        TupleExpression(c) => p_tuple_expr(c, env, location),
        FunctionCallExpression(c) => p_function_call_expr(c, env),
        FunctionPointerExpression(c) => p_function_pointer_expr(node, c, env),
        QualifiedName(_) => p_qualified_name(node, env, location),
        VariableExpression(c) => p_variable_expr(c, env, pos),
        PipeVariableExpression(_) => p_pipe_variable_expr(pos),
        InclusionExpression(c) => p_inclusion_expr(c, env),
        MemberSelectionExpression(c) => p_obj_get(location, &c.object, &c.operator, &c.name, env),
        SafeMemberSelectionExpression(c) => {
            p_obj_get(location, &c.object, &c.operator, &c.name, env)
        }
        EmbeddedMemberSelectionExpression(c) => {
            p_obj_get(location, &c.object, &c.operator, &c.name, env)
        }
        PrefixUnaryExpression(_) | PostfixUnaryExpression(_) | DecoratedExpression(_) => {
            p_pre_post_unary_decorated_expr(node, env, pos)
        }
        BinaryExpression(c) => p_binary_expr(c, env, pos, location),
        Token(t) => p_token(node, t, env, location),
        YieldExpression(c) => p_yield_expr(node, c, env, pos, location),
        ScopeResolutionExpression(c) => p_scope_resolution_expr(node, c, env, pos, location),
        CastExpression(c) => p_cast_expr(c, env),
        PrefixedCodeExpression(c) => p_prefixed_code_expr(node, c, env),
        ETSpliceExpression(c) => p_et_splice_expr(&c.expression, env, location),
        ConditionalExpression(c) => p_conditional_expr(c, env),
        SubscriptExpression(c) => p_subscript_expr(c, env),
        EmbeddedSubscriptExpression(c) => p_embedded_subscript_expr(c, env, location),
        ShapeExpression(c) => p_shape_expr(c, env, location),
        ObjectCreationExpression(c) => p_expr_recurse(location, &c.object, env, Some(pos)),
        ConstructorCall(c) => p_constructor_call(node, c, env, pos),
        GenericTypeSpecifier(c) => p_generic_type_specifier(c, env),
        LiteralExpression(c) => p_expr_lit(location, node, &c.expression, env),
        PrefixedStringExpression(c) => p_prefixed_string_expr(node, c, env),
        IsExpression(c) => p_is_expr(&c.left_operand, &c.right_operand, env),
        AsExpression(c) => p_as_expr(&c.left_operand, &c.right_operand, env, false),
        NullableAsExpression(c) => p_as_expr(&c.left_operand, &c.right_operand, env, true),
        UpcastExpression(c) => p_upcast_expr(&c.left_operand, &c.right_operand, env),
        AnonymousFunction(c) => p_anonymous_function(node, c, env),
        AwaitableCreationExpression(c) => p_awaitable_creation_expr(c, env, pos),
        XHPExpression(c) if c.open.is_xhp_open() => p_xhp_expr(c, env),
        EnumClassLabelExpression(c) => p_enum_class_label_expr(c, env),
        PackageExpression(p) => p_package_expr(p, env),
        NameofExpression(p) => p_nameof(p, env),
        _ => {
            raise_missing_syntax("expression", node, env);
            Ok(Expr_::Null)
        }
    }
}

fn p_lambda_expression<'a>(
    c: &'a LambdaExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
) -> Result<Expr_> {
    let suspension_kind = mk_suspension_kind(&c.async_);
    let (tparams, params, (ctxs, unsafe_ctxs), readonly_ret, ret) = match &c.signature.children {
        LambdaSignature(c) => {
            let tparams = p_hint_tparam_l(&c.type_parameters, env)?;
            let params = could_map(&c.parameters, env, p_fun_param)?;
            let readonly_ret = map_optional(&c.readonly_return, env, p_readonly)?;
            let ctxs = p_contexts(
                &c.contexts,
                env,
                // TODO(coeffects) Lambdas may be able to support this:: contexts
                Some((&syntax_error::lambda_effect_polymorphic("A lambda"), false)),
            );
            let unsafe_ctxs = ctxs.clone();
            let ret = map_optional(&c.type_, env, p_hint)?;
            (tparams, params, (ctxs, unsafe_ctxs), readonly_ret, ret)
        }
        Token(_) => {
            let ast::Id(p, n) = pos_name(&c.signature, env)?;
            (
                vec![],
                vec![ast::FunParam {
                    annotation: (),
                    type_hint: ast::TypeHint((), None),
                    pos: p,
                    name: n,
                    info: ast::FunParamInfo::ParamRequired,
                    callconv: ast::ParamKind::Pnormal,
                    readonly: None,
                    splat: None,
                    named: None,
                    user_attributes: Default::default(),
                    visibility: None,
                }],
                (None, None),
                None,
                None,
            )
        }
        _ => missing_syntax("lambda signature", &c.signature, env)?,
    };

    let (body, yield_) = if !c.body.is_compound_statement() {
        env.reset_for_function_body(&c.body, suspension_kind, p_function_body)?
    } else {
        let mut env1 = Env::clone_and_unset_toplevel_if_toplevel(env);
        env1.as_mut()
            .reset_for_function_body(&c.body, suspension_kind, p_function_body)?
    };
    let external = c.body.is_external();
    let fun = ast::Fun_ {
        span: pos,
        readonly_this: None, // filled in by mk_unop
        tparams,
        annotation: (),
        readonly_ret,
        ret: ast::TypeHint((), ret),
        body: ast::FuncBody { fb_ast: body },
        fun_kind: mk_fun_kind(suspension_kind, yield_),
        params,
        ctxs,
        unsafe_ctxs,
        user_attributes: p_user_attributes(&c.attribute_spec, env),
        external,
        hidden: false,
        doc_comment: None,
    };
    Ok(Expr_::mk_lfun(fun, vec![]))
}

fn p_collection_literal_expr<'a>(
    node: S<'a>,
    c: &'a CollectionLiteralExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let (collection_name, hints) = match &c.name.children {
        SimpleTypeSpecifier(c) => (pos_name(&c.specifier, env)?, None),
        GenericTypeSpecifier(c) => {
            let hints = expand_type_args(&c.argument_list, env)?;
            let hints = check_intrinsic_type_arg_varity(node, env, hints);
            (pos_name(&c.class_type, env)?, hints)
        }
        _ => (pos_name(&c.name, env)?, None),
    };
    Ok(Expr_::mk_collection(
        collection_name,
        hints,
        could_map(&c.initializers, env, p_afield)?,
    ))
}

fn p_varray_intrinsic_expr<'a>(
    node: S<'a>,
    c: &'a VarrayIntrinsicExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let hints = expand_type_args(&c.explicit_type, env)?;
    let hints = check_intrinsic_type_arg_varity(node, env, hints);
    let targ = match hints {
        Some(ast::CollectionTarg::CollectionTV(ty)) => Some(ty),
        None => None,
        _ => missing_syntax("VarrayIntrinsicExpression type args", node, env)?,
    };
    let vec = Expr_::mk_val_collection(
        (p_pos(&c.keyword, env), aast::VcKind::Vec),
        targ,
        could_map(&c.members, env, p_expr)?,
    );
    Ok(vec)
}

fn p_darray_intrinsic_expr<'a>(
    node: S<'a>,
    c: &'a DarrayIntrinsicExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let hints = expand_type_args(&c.explicit_type, env)?;
    let hints = check_intrinsic_type_arg_varity(node, env, hints);
    let targ = match hints {
        Some(ast::CollectionTarg::CollectionTKV(tk, tv)) => Some((tk, tv)),
        None => None,
        _ => missing_syntax("DarrayIntrinsicExpression type args", node, env)?,
    };
    let dict = Expr_::mk_key_val_collection(
        (p_pos(&c.keyword, env), aast::KvcKind::Dict),
        targ,
        could_map(&c.members, env, p_field)?,
    );
    Ok(dict)
}

fn p_list_expr<'a>(
    c: &'a ListExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    /* TODO: Or tie in with other intrinsics and post-process to List */
    let p_binder_or_ignore = |n: S<'a>, e: &mut Env<'a>| -> Result<ast::Expr> {
        match &n.children {
            Missing => Ok(Expr::new((), e.mk_none_pos(), Expr_::Omitted)),
            _ => p_expr(n, e),
        }
    };
    Ok(Expr_::List(could_map(&c.members, env, p_binder_or_ignore)?))
}

fn p_tuple_expr<'a>(
    c: &'a TupleExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    location: ExprLocation,
) -> Result<Expr_> {
    let has_ellipsis = !c.ellipsis.is_missing();
    if location == ExprLocation::LvaluePosition {
        // Lvalue context: produce DestructureTuple
        let pos = p_pos(&c.keyword, env);
        let mut seen_optional = false;
        let mut dt_entries = Vec::new();
        for node in c.items.syntax_node_to_list_skip_separator() {
            let mut entry = p_destructure_tuple_entry(node, env)?;
            if seen_optional && !entry.optional {
                raise_parsing_error(
                    node,
                    env,
                    "There must be no required elements following an optional element",
                );
                // Mark as optional to maintain the invariant that all entries
                // after the first optional are also optional. The type
                // checker and emitter iterate entries positionally.
                entry.optional = true;
            }
            seen_optional |= entry.optional;
            dt_entries.push(entry);
        }
        Ok(Expr_::DestructureTuple(Box::new(aast::DestructureTuple {
            pos,
            entries: dt_entries,
            ellipsis: has_ellipsis,
        })))
    } else {
        // Expression context: produce Tuple
        if has_ellipsis {
            raise_parsing_error(
                &c.ellipsis,
                env,
                "Ellipsis `...` is only valid in tuple destructuring patterns",
            );
        }
        Ok(Expr_::mk_tuple(could_map(&c.items, env, p_expr)?))
    }
}

fn p_function_call_expr<'a>(
    c: &'a FunctionCallExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let recv = &c.receiver;
    let args = &c.argument_list;
    let targs = match (&recv.children, &c.type_args.children) {
        (_, TypeArguments(c)) => could_map(&c.types, env, p_targ)?,
        /* TODO might not be needed */
        (GenericTypeSpecifier(c), _) => match &c.argument_list.children {
            TypeArguments(c) => could_map(&c.types, env, p_targ)?,
            _ => vec![],
        },
        _ => vec![],
    };

    // Mark expression as CallReceiver so that we can correctly set
    // PropOrMethod field in ObjGet and ClassGet
    let recv = p_expr_with_loc(ExprLocation::CallReceiver, recv, env, None)?;
    let (args, unpacked_arg) = split_args_unpacked_arg(args, env)?;

    Ok(Expr_::mk_call(ast::CallExpr {
        func: recv,
        targs,
        args,
        unpacked_arg,
    }))
}

fn p_function_pointer_expr<'a>(
    node: S<'a>,
    c: &'a FunctionPointerExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let targs = match &c.type_args.children {
        TypeArguments(c) => could_map(&c.types, env, p_targ)?,
        _ => vec![],
    };

    let recv = p_expr(&c.receiver, env)?;

    match &recv.2 {
        Expr_::Id(id) => Ok(Expr_::mk_function_pointer(
            aast::FunctionPtrId::FPId(*(id.to_owned())),
            targs,
            aast::FunctionPointerSource::Code,
        )),
        Expr_::ClassConst(c) => match &(c.0).2 {
            aast::ClassId_::CIexpr(Expr(_, _, Expr_::Id(_)))
            | aast::ClassId_::CIself
            | aast::ClassId_::CIparent
            | aast::ClassId_::CIreified(_) => Ok(Expr_::mk_function_pointer(
                aast::FunctionPtrId::FPClassConst(c.0.to_owned(), c.1.to_owned()),
                targs,
                aast::FunctionPointerSource::Code,
            )),
            _ => {
                raise_parsing_error(node, env, &syntax_error::function_pointer_bad_recv);
                missing_syntax("function or static method", node, env)
            }
        },
        _ => {
            raise_parsing_error(node, env, &syntax_error::function_pointer_bad_recv);
            missing_syntax("function or static method", node, env)
        }
    }
}

fn p_qualified_name<'a>(node: S<'a>, env: &mut Env<'a>, location: ExprLocation) -> Result<Expr_> {
    match location {
        ExprLocation::InDoubleQuotedString => {
            let ast::Id(_, n) = pos_qualified_name(node, env)?;
            Ok(Expr_::String(n.into()))
        }
        _ => Ok(Expr_::mk_id(pos_qualified_name(node, env)?)),
    }
}

fn p_variable_expr<'a>(
    c: &'a VariableExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
) -> Result<Expr_> {
    Ok(Expr_::mk_lvar(lid_from_pos_name(pos, &c.expression, env)?))
}

fn p_pipe_variable_expr(pos: Pos) -> Result<Expr_> {
    Ok(Expr_::mk_lvar(mk_lid(
        pos,
        special_idents::DOLLAR_DOLLAR.into(),
    )))
}

fn p_inclusion_expr<'a>(
    c: &'a InclusionExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    Ok(Expr_::mk_import(
        p_import_flavor(&c.require, env)?,
        p_expr(&c.filename, env)?,
    ))
}

fn p_pre_post_unary_decorated_expr<'a>(node: S<'a>, env: &mut Env<'a>, pos: Pos) -> Result<Expr_> {
    let (operand, op, postfix) = match &node.children {
        PrefixUnaryExpression(c) => (&c.operand, &c.operator, false),
        PostfixUnaryExpression(c) => (&c.operand, &c.operator, true),
        DecoratedExpression(c) => (&c.expression, &c.decorator, false),
        _ => missing_syntax("unary exppr", node, env)?,
    };

    /**
     * FFP does not destinguish between ++$i and $i++ on the level of token
     * kind annotation. Prevent duplication by switching on `postfix` for
     * the two operatores for which AST /does/ differentiate between
     * fixities.
     */
    use ast::Uop::*;
    let mk_unop = |op, e| Ok(Expr_::mk_unop(op, e));
    let op_kind = token_kind(op);
    if let Some(TK::At) = op_kind {
        raise_parsing_error(op, env, &syntax_error::no_silence);
        if env.is_codegen() {
            let expr = p_expr(operand, env)?;
            mk_unop(Usilence, expr)
        } else {
            let expr = p_expr_with_loc(ExprLocation::TopLevel, operand, env, Some(pos))?;
            Ok(expr.2)
        }
    } else {
        let expr = p_expr(operand, env)?;
        match op_kind {
            Some(TK::PlusPlus) if postfix => mk_unop(Upincr, expr),
            Some(TK::MinusMinus) if postfix => mk_unop(Updecr, expr),
            Some(TK::PlusPlus) => mk_unop(Uincr, expr),
            Some(TK::MinusMinus) => mk_unop(Udecr, expr),
            Some(TK::Exclamation) => mk_unop(Unot, expr),
            Some(TK::Tilde) => mk_unop(Utild, expr),
            Some(TK::Plus) => mk_unop(Uplus, expr),
            Some(TK::Minus) => mk_unop(Uminus, expr),
            Some(TK::Await) => {
                env.found_await = true;
                Ok(Expr_::mk_await(expr))
            }
            Some(TK::Delay) => Ok(Expr_::mk_delay(expr)),
            Some(TK::Readonly) => Ok(process_readonly_expr(expr)),
            Some(TK::Clone) => Ok(Expr_::mk_clone(expr)),
            Some(TK::Print) => Ok(Expr_::mk_call(ast::CallExpr {
                func: Expr::new(
                    (),
                    pos.clone(),
                    Expr_::mk_id(ast::Id(pos, pre_namespaced_functions::ECHO.into())),
                ),
                targs: vec![],
                args: vec![aast::Argument::Anormal(expr)],
                unpacked_arg: None,
            })),
            Some(TK::Dollar) => {
                raise_parsing_error(op, env, &syntax_error::invalid_variable_name);
                Ok(Expr_::Omitted)
            }
            _ => missing_syntax("unary operator", node, env),
        }
    }
}

fn p_binary_expr<'a>(
    c: &'a BinaryExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
    location: ExprLocation,
) -> Result<Expr_> {
    use ExprLocation::*;
    let lhs_location = if token_kind(&c.operator) == Some(TK::Equal) {
        LvaluePosition
    } else {
        TopLevel
    };
    let left = p_expr_with_loc(lhs_location, &c.left_operand, env, None)?;
    let rlocation = match (token_kind(&c.operator), location) {
        (Some(TK::Equal), AsStatement) => RightOfAssignment,
        (Some(TK::Equal), UsingStatement) => RightOfAssignmentInUsingStatement,
        _ => TopLevel,
    };
    let right = p_expr_with_loc(rlocation, &c.right_operand, env, None)?;
    p_bop(pos, &c.operator, left, right, env)
}

fn p_token<'a>(
    node: S<'a>,
    t: &'a PositionedToken<'_>,
    env: &mut Env<'a>,
    location: ExprLocation,
) -> Result<Expr_> {
    use ExprLocation::*;
    match (location, t.kind()) {
        (MemberSelect, TK::Variable) => mk_lvar(node, env),
        (InDoubleQuotedString, TK::HeredocStringLiteral)
        | (InDoubleQuotedString, TK::HeredocStringLiteralHead)
        | (InDoubleQuotedString, TK::HeredocStringLiteralTail) => Ok(Expr_::String(
            wrap_unescaper(text_str(node, env), unescape_heredoc, p_pos(node, env))?,
        )),
        (InDoubleQuotedString, _) => Ok(Expr_::String(wrap_unescaper(
            text_str(node, env),
            unesc_dbl,
            p_pos(node, env),
        )?)),
        (MemberSelect, _)
        | (TopLevel, _)
        | (AsStatement, _)
        | (UsingStatement, _)
        | (RightOfAssignment, _)
        | (RightOfAssignmentInUsingStatement, _)
        | (RightOfReturn, _)
        | (CallReceiver, _)
        | (LvaluePosition, _) => Ok(Expr_::mk_id(pos_name(node, env)?)),
    }
}

fn p_yield_expr<'a>(
    node: S<'a>,
    c: &'a YieldExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
    location: ExprLocation,
) -> Result<Expr_> {
    use ExprLocation::*;
    env.found_yield = true;
    if location != AsStatement
        && location != RightOfAssignment
        && location != RightOfAssignmentInUsingStatement
    {
        raise_parsing_error(node, env, &syntax_error::invalid_yield);
    }
    if c.operand.is_missing() {
        Ok(Expr_::mk_yield(ast::Afield::AFvalue(Expr::new(
            (),
            pos,
            Expr_::Null,
        ))))
    } else {
        Ok(Expr_::mk_yield(p_afield(&c.operand, env)?))
    }
}

fn p_scope_resolution_expr<'a>(
    node: S<'a>,
    c: &'a ScopeResolutionExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
    location: ExprLocation,
) -> Result<Expr_> {
    let qual = p_expr(&c.qualifier, env)?;
    if let Expr_::Id(id) = &qual.2 {
        fail_if_invalid_reified_generic(node, env, &id.1);
    }
    match &c.name.children {
        Token(token) if token.kind() == TK::Variable => {
            if location == ExprLocation::CallReceiver {
                Err(Error::ParsingError {
                    message: String::from("Dynamic calls to static methods are banned."),
                    pos,
                })
            } else {
                let class_id = expr_to_class_id(env, pos, qual)?;
                let ast::Id(p, name) = pos_name(&c.name, env)?;
                Ok(Expr_::mk_class_get(
                    class_id,
                    (p, name),
                    ast::PropOrMethod::IsProp,
                ))
            }
        }
        _ => {
            let class_id = expr_to_class_id(env, pos, qual)?;
            let Expr(_, p, expr_) = p_expr(&c.name, env)?;
            match expr_ {
                Expr_::String(id) => Ok(Expr_::mk_class_const(
                    class_id,
                    (
                        p.clone(),
                        String::from_utf8(id.into()).map_err(|e| Error::ParsingError {
                            message: e.to_string(),
                            pos: p,
                        })?,
                    ),
                )),
                Expr_::Id(id) => {
                    let ast::Id(p, n) = *id;
                    Ok(Expr_::mk_class_const(class_id, (p, n)))
                }
                Expr_::Lvar(id) if location != ExprLocation::CallReceiver => {
                    let ast::Lid(p, (_, n)) = *id;
                    Ok(Expr_::mk_class_get(
                        class_id,
                        (p, n),
                        ast::PropOrMethod::IsProp,
                    ))
                }
                _ => missing_syntax("static property or method name", &c.name, env),
            }
        }
    }
}

fn p_cast_expr<'a>(
    c: &'a CastExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    if env.is_typechecker() && token_kind(&c.type_) == Some(TK::Bool) {
        raise_parsing_error(&c.type_, env, &syntax_error::no_bool_cast);
    }
    Ok(Expr_::mk_cast(
        p_hint(&c.type_, env)?,
        p_expr(&c.operand, env)?,
    ))
}

fn p_prefixed_code_expr<'a>(
    node: S<'a>,
    c: &'a PrefixedCodeExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let mut clear_et_class_at_end = false;
    let missing = c.prefix.is_missing();
    let is_nested = env.expression_tree_class.is_some();
    let nested_id = match (pos_name(&c.prefix, env), &env.expression_tree_class) {
        (Ok(nested_id), Some(enclosing_id)) => {
            if !missing {
                if enclosing_id.1 != nested_id.1 {
                    raise_parsing_error(
                        node,
                        env,
                        &format!(
                            "{} {} inside of {}",
                            syntax_error::expression_tree_name_mismatch,
                            nested_id.1,
                            enclosing_id.1,
                        ),
                    );
                };
                Ok(nested_id)
            } else {
                Ok(enclosing_id.clone())
            }
        }
        (Ok(nested_id), None) => {
            let nested_id = if !missing {
                nested_id
            } else {
                Id(nested_id.0, "MISSING_DSL".to_string())
            };
            clear_et_class_at_end = true;
            env.expression_tree_class = Some(nested_id.clone());
            Ok(nested_id)
        }
        (Err(e), _) => Err(e),
    }?;
    env.in_expr_tree = true;
    let mut old_et_pos = None;
    std::mem::swap(&mut old_et_pos, &mut env.enclosing_et_pos);
    env.enclosing_et_pos = Some(p_pos(&c.body, env));
    let src_expr = if !c.body.is_compound_statement() {
        p_expr(&c.body, env)?
    } else {
        let pos = p_pos(&c.body, env);
        let suspension_kind = SuspensionKind::SKSync;
        // Take the body and create a no argument lambda expression
        let body = p_function_body(&c.body, env)?;
        let external = c.body.is_external();
        let fun = ast::Fun_ {
            span: pos.clone(),
            readonly_this: None, // filled in by mk_unop
            tparams: vec![],
            annotation: (),
            readonly_ret: None,
            ret: ast::TypeHint((), None),
            body: ast::FuncBody { fb_ast: body },
            fun_kind: mk_fun_kind(suspension_kind, false),
            params: vec![],
            ctxs: None,
            unsafe_ctxs: None,
            user_attributes: ast::UserAttributes(vec![]),
            external,
            hidden: false,
            doc_comment: None,
        };
        let recv = ast::Expr::new((), pos.clone(), Expr_::mk_lfun(fun, vec![]));

        // Immediately invoke the lambda by wrapping in a call expression node
        let expr = Expr_::mk_call(ast::CallExpr {
            func: recv,
            targs: vec![],
            args: vec![],
            unpacked_arg: None,
        });
        ast::Expr::new((), pos, expr)
    };
    std::mem::swap(&mut old_et_pos, &mut env.enclosing_et_pos);
    let desugar_result = desugar(&nested_id, src_expr, env, is_nested);
    for (pos, msg) in desugar_result.errors {
        raise_parsing_error_pos(&pos, env, &msg);
    }
    if clear_et_class_at_end {
        env.expression_tree_class = None;
    }
    // Since expression trees can't directly nest, we don't need to restore the
    // old value, it must be false.
    env.in_expr_tree = false;
    Ok(desugar_result.expr.2)
}

fn p_et_splice_expr<'a>(expr: S<'a>, env: &mut Env<'a>, location: ExprLocation) -> Result<Expr_> {
    // Clear out found awaits since we want to see if there is one in this splice expression.
    let old_found_await = env.found_await;
    env.found_await = false;
    env.in_expr_tree = false;
    let inner_pos = p_pos(expr, env);
    let inner_expr_ = p_expr_recurse(location, expr, env, None)?;
    let spliced_expr = ast::Expr::new((), inner_pos, inner_expr_);
    let mut macro_vars = ETVars::default();
    let _ = macro_vars.visit_expr(&mut (), &spliced_expr);
    let macro_variables = if macro_vars.vars.is_empty() {
        None
    } else {
        Some(macro_vars.vars)
    };
    let contains_await = env.found_await;
    env.found_await |= old_found_await;
    // Since splices can't directly nest, we don't need to restore the old value,
    // it must be true
    env.in_expr_tree = true;
    Ok(Expr_::mk_etsplice(aast::EtSplice {
        spliced_expr,
        contains_await,
        extract_client_type: true,
        macro_variables,
        temp_lid: None, // This gets replaced during expression tree desugaring
    }))
}

// visitor to collect all of the free variables from sub-expressions
// that are expression trees
#[derive(Default)]
struct ETVars {
    vars: Vec<aast::Lid>,
}

impl<'ast> Visitor<'ast> for ETVars {
    type Params = AstParams<(), ()>;

    fn object(&mut self) -> &mut dyn Visitor<'ast, Params = Self::Params> {
        self
    }

    fn visit_expression_tree(
        &mut self,
        _env: &mut (),
        et: &'ast aast::ExpressionTree<(), ()>,
    ) -> Result<(), ()> {
        if let Some(free_vars) = &et.free_vars {
            for v in free_vars.iter() {
                self.vars.push(v.clone());
            }
        }
        Ok(())
    }
}

fn p_conditional_expr<'a>(
    c: &'a ConditionalExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let alter = p_expr(&c.alternative, env)?;
    let consequence = map_optional(&c.consequence, env, p_expr)?;
    let condition = p_expr(&c.test, env)?;
    Ok(Expr_::mk_eif(condition, consequence, alter))
}

fn p_subscript_expr<'a>(
    c: &'a SubscriptExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    Ok(Expr_::mk_array_get(
        p_expr(&c.receiver, env)?,
        map_optional(&c.index, env, p_expr)?,
    ))
}

fn p_embedded_subscript_expr<'a>(
    c: &'a EmbeddedSubscriptExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    location: ExprLocation,
) -> Result<Expr_> {
    Ok(Expr_::mk_array_get(
        p_expr(&c.receiver, env)?,
        map_optional(&c.index, env, |n, e| p_expr_with_loc(location, n, e, None))?,
    ))
}

fn p_constructor_call<'a>(
    node: S<'a>,
    c: &'a ConstructorCallChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
) -> Result<Expr_> {
    let (args, unpacked_arg) = split_args_unpacked_arg(&c.argument_list, env)?;
    let (e, hl) = match &c.type_.children {
        GenericTypeSpecifier(c) => {
            let name = pos_name(&c.class_type, env)?;
            let hints = match &c.argument_list.children {
                TypeArguments(c) => could_map(&c.types, env, p_targ)?,
                _ => missing_syntax("generic type arguments", &c.argument_list, env)?,
            };
            (mk_id_expr(name), hints)
        }
        SimpleTypeSpecifier(_) => {
            let name = pos_name(&c.type_, env)?;
            (mk_id_expr(name), vec![])
        }
        _ => (p_expr(&c.type_, env)?, vec![]),
    };
    if let Expr_::Id(name) = &e.2 {
        fail_if_invalid_reified_generic(node, env, &name.1);
        fail_if_invalid_class_creation(node, env, &name.1);
    }
    let class_id = expr_to_class_id(env, pos, e)?;
    if let aast::ClassId_::CIreified(_) = class_id.2
        && !hl.is_empty()
    {
        raise_parsing_error(&c.argument_list, env, &syntax_error::targs_not_allowed);
    }
    Ok(Expr_::mk_new(class_id, hl, args, unpacked_arg, ()))
}

fn p_generic_type_specifier<'a>(
    c: &'a GenericTypeSpecifierChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    if !c.argument_list.is_missing() {
        raise_parsing_error(&c.argument_list, env, &syntax_error::targs_not_allowed)
    }
    Ok(Expr_::mk_id(pos_name(&c.class_type, env)?))
}

/// Lower a shape field for destructuring patterns. Handles:
/// - FieldInitializer: 'key' => $var
/// - VariableExpression: $var (punning)
/// - PrefixUnaryExpression(?, inner): ?'key' => $var or ?$var (optional)
fn p_destructure_shape_field<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<aast::DestructureShapeField<(), ()>> {
    match &node.children {
        PrefixUnaryExpression(c) if token_kind(&c.operator) == Some(TK::Question) => {
            // ?'key' => $var or ?$var (optional field)
            let inner = &c.operand;
            let mut field = p_destructure_shape_field(inner, env)?;
            field.optional = true;
            Ok(field)
        }
        FieldInitializer(c) => {
            let name = p_shape_field_name(&c.name, env)?;
            let value = p_expr_with_loc(ExprLocation::LvaluePosition, &c.value, env, None)?;
            let target = expr_to_destructure_target(value, env);
            Ok(aast::DestructureShapeField {
                optional: false,
                name,
                target,
            })
        }
        VariableExpression(c) => {
            // Punning in destructuring patterns: shape($foo) = $s is sugar for shape('foo' => $foo) = $s
            let pos = p_pos(node, env);
            let var_name = text_str(&c.expression, env);
            let field_name = var_name.strip_prefix('$').unwrap_or(var_name);
            let name = ast::ShapeFieldName::SFlitStr((pos.clone(), field_name.to_string().into()));
            let value = p_expr_with_loc(ExprLocation::LvaluePosition, node, env, None)?;
            let target = expr_to_destructure_target(value, env);
            Ok(aast::DestructureShapeField {
                optional: false,
                name,
                target,
            })
        }
        _ => missing_syntax("shape destructure field", node, env),
    }
}

/// Lower a tuple entry for destructuring patterns. Handles:
/// - PrefixUnaryExpression(optional, inner): optional $var (optional entry)
/// - Any other expression: plain entry
fn p_destructure_tuple_entry<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<aast::DestructureTupleEntry<(), ()>> {
    match &node.children {
        PrefixUnaryExpression(c) if token_kind(&c.operator) == Some(TK::Optional) => {
            let value = p_expr_with_loc(ExprLocation::LvaluePosition, &c.operand, env, None)?;
            let target = expr_to_destructure_target(value, env);
            Ok(aast::DestructureTupleEntry {
                optional: true,
                target,
            })
        }
        _ => {
            let value = p_expr_with_loc(ExprLocation::LvaluePosition, node, env, None)?;
            let target = expr_to_destructure_target(value, env);
            Ok(aast::DestructureTupleEntry {
                optional: false,
                target,
            })
        }
    }
}

/// Convert an Expr to a DestructureTarget for destructuring patterns.
/// The only valid targets per the spec are: lvar, wildcard (_),
/// destructure_shape, and destructure_tuple.
fn expr_to_destructure_target<'a>(
    value: ast::Expr,
    env: &mut Env<'a>,
) -> aast::DestructureTarget<(), ()> {
    match value.2 {
        Expr_::Lvar(lid) => aast::DestructureTarget((), (), aast::DestructureTarget_::DtLvar(*lid)),
        Expr_::Lplaceholder(pos) => {
            aast::DestructureTarget((), (), aast::DestructureTarget_::DtWildcard(*pos))
        }
        Expr_::DestructureShape(ds) => {
            aast::DestructureTarget((), (), aast::DestructureTarget_::DtShape(*ds))
        }
        Expr_::DestructureTuple(dt) => {
            aast::DestructureTarget((), (), aast::DestructureTarget_::DtTuple(*dt))
        }
        Expr_::Id(sid) if sid.1 == "_" => {
            aast::DestructureTarget((), (), aast::DestructureTarget_::DtWildcard(value.1))
        }
        _ => {
            // Invalid target in destructuring pattern (e.g., $obj->prop, f(), etc.).
            let pos = value.1;
            raise_parsing_error_pos(
                &pos,
                env,
                "Only variables, wildcards, and nested shape/tuple patterns are allowed in destructuring patterns",
            );
            aast::DestructureTarget((), (), aast::DestructureTarget_::DtWildcard(pos))
        }
    }
}

fn p_shape_expr<'a>(
    c: &'a ShapeExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    location: ExprLocation,
) -> Result<Expr_> {
    let has_ellipsis = !c.ellipsis.is_missing();
    if location == ExprLocation::LvaluePosition {
        // Lvalue context: produce DestructureShape
        let pos = p_pos(&c.keyword, env);
        let ds_fields = could_map(&c.fields, env, |n, e| p_destructure_shape_field(n, e))?;
        Ok(Expr_::DestructureShape(Box::new(aast::DestructureShape {
            pos,
            fields: ds_fields,
            ellipsis: has_ellipsis,
        })))
    } else {
        // Expression context: produce Shape
        if has_ellipsis {
            raise_parsing_error(
                &c.ellipsis,
                env,
                "Ellipsis `...` is only valid in shape destructuring patterns",
            );
        }
        Ok(Expr_::Shape(could_map(&c.fields, env, |n, e| {
            if let ShapeSplatExpression(sc) = n.children {
                let expr = p_expr(&sc.expression, e)?;
                Ok(ast::ShapeExprField::SFSplat(expr))
            } else {
                let (name, expr) = map_shape_expression_field(n, e, p_expr)?;
                Ok(ast::ShapeExprField::SFField(name, expr))
            }
        })?))
    }
}

fn p_prefixed_string_expr<'a>(
    node: S<'a>,
    c: &'a PrefixedStringExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    /* Temporarily allow only`re`- prefixed strings */
    let name_text = text(&c.name, env);
    if name_text != "re" {
        raise_parsing_error(node, env, &syntax_error::non_re_prefix);
    }
    Ok(Expr_::mk_prefixed_string(name_text, p_expr(&c.str, env)?))
}

fn p_is_expr<'a>(left: S<'a>, right: S<'a>, env: &mut Env<'a>) -> Result<Expr_> {
    Ok(Expr_::mk_is(p_expr(left, env)?, p_hint(right, env)?))
}

fn p_as_expr<'a>(left: S<'a>, right: S<'a>, env: &mut Env<'a>, is_nullable: bool) -> Result<Expr_> {
    let expr = p_expr(left, env)?;
    let hint = p_hint(right, env)?;
    Ok(Expr_::mk_as(oxidized::nast::As_ {
        expr,
        hint,
        is_nullable,
        enforce_deep: true,
    }))
}

fn p_upcast_expr<'a>(left: S<'a>, right: S<'a>, env: &mut Env<'a>) -> Result<Expr_> {
    Ok(Expr_::mk_upcast(p_expr(left, env)?, p_hint(right, env)?))
}

fn p_use_var<'a>(n: S<'a>, e: &mut Env<'a>) -> Result<ast::CaptureLid> {
    match &n.children {
        Token(_) => {
            let lid = mk_name_lid(n, e)?;
            Ok(ast::CaptureLid((), lid))
        }
        _ => missing_syntax("use variable", n, e),
    }
}

fn p_anonymous_function<'a>(
    node: S<'a>,
    c: &'a AnonymousFunctionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let suspension_kind = mk_suspension_kind(&c.async_keyword);
    let ctxs = p_contexts(
        &c.ctx_list,
        env,
        // TODO(coeffects) Anonymous functions may be able to support this:: contexts
        Some((
            &syntax_error::lambda_effect_polymorphic("An anonymous function"),
            false,
        )),
    );

    let unsafe_ctxs = ctxs.clone();
    let p_use = |n: S<'a>, e: &mut Env<'a>| match &n.children {
        AnonymousFunctionUseClause(c) => could_map(&c.variables, e, p_use_var),
        _ => Ok(vec![]),
    };
    let (body, yield_) = {
        let mut env1 = Env::clone_and_unset_toplevel_if_toplevel(env);
        env1.as_mut()
            .reset_for_function_body(&c.body, suspension_kind, p_function_body)?
    };
    let doc_comment = extract_docblock(node, env).or_else(|| env.top_docblock().clone());
    let user_attributes = p_user_attributes(&c.attribute_spec, env);
    let external = c.body.is_external();
    let params = could_map(&c.parameters, env, p_fun_param)?;
    let tparams = p_hint_tparam_l(&c.type_parameters, env)?;
    let fun = ast::Fun_ {
        span: p_pos(node, env),
        readonly_this: None, // set in process_readonly_expr
        tparams,
        annotation: (),
        readonly_ret: map_optional(&c.readonly_return, env, p_readonly)?,
        ret: ast::TypeHint((), map_optional(&c.type_, env, p_hint)?),
        body: ast::FuncBody { fb_ast: body },
        fun_kind: mk_fun_kind(suspension_kind, yield_),
        params,
        ctxs,
        unsafe_ctxs,
        user_attributes,
        external,
        hidden: false,
        doc_comment,
    };
    let use_ = p_use(&c.use_, env).unwrap_or_else(|_| vec![]);
    Ok(Expr_::mk_efun(ast::Efun {
        fun,
        use_,
        closure_class_name: None,
        is_expr_tree_virtual_expr: false,
    }))
}

fn p_awaitable_creation_expr<'a>(
    c: &'a AwaitableCreationExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
    pos: Pos,
) -> Result<Expr_> {
    let suspension_kind = mk_suspension_kind(&c.async_);
    let (blk, yld) =
        env.reset_for_function_body(&c.compound_statement, suspension_kind, p_function_body)?;
    let user_attributes = p_user_attributes(&c.attribute_spec, env);
    let external = c.compound_statement.is_external();
    let body = ast::Fun_ {
        span: pos.clone(),
        annotation: (),
        readonly_this: None, // set in process_readonly_expr
        readonly_ret: None,  // TODO: awaitable creation expression
        tparams: vec![],
        ret: ast::TypeHint((), None),
        body: ast::FuncBody {
            fb_ast: if blk.is_empty() {
                let pos = p_pos(&c.compound_statement, env);
                ast::Block(vec![ast::Stmt::noop(pos)])
            } else {
                blk
            },
        },
        fun_kind: mk_fun_kind(suspension_kind, yld),
        params: vec![],
        ctxs: None,        // TODO(T70095684)
        unsafe_ctxs: None, // TODO(T70095684)
        user_attributes,
        external,
        hidden: false,
        doc_comment: None,
    };
    Ok(Expr_::mk_call(ast::CallExpr {
        func: Expr::new((), pos, Expr_::mk_lfun(body, vec![])),
        targs: vec![],
        args: vec![],
        unpacked_arg: None,
    }))
}

fn p_xhp_expr<'a>(
    c: &'a XHPExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    if let XHPOpen(c1) = &c.open.children {
        let name = pos_name(&c1.name, env)?;
        let attrs = could_map(&c1.attributes, env, p_xhp_attr)?;
        let exprs = aggregate_xhp_tokens(env, &c.body)?
            .iter()
            .map(|n| p_xhp_embedded(n, env, unesc_xhp))
            .collect::<Result<Vec<_>, _>>()?;

        let id = if env.empty_ns_env.disable_xhp_element_mangling {
            ast::Id(name.0, name.1)
        } else {
            ast::Id(name.0, String::from(":") + &name.1)
        };

        Ok(Expr_::mk_xml(
            // TODO: update pos_name to support prefix
            id, attrs, exprs,
        ))
    } else {
        missing_syntax("XHP open", &c.open, env)
    }
}

fn p_enum_class_label_expr<'a>(
    c: &'a EnumClassLabelExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    use syntax_kind::SyntaxKind;
    /* Foo#Bar can be the following:
     * - short version: Foo is None/missing and we only have #Bar
     * - Foo is a name -> fully qualified Foo#Bar
     */
    let ast::Id(_label_pos, label_name) = pos_name(&c.expression, env)?;
    let qual = if c.qualifier.is_missing() {
        None
    } else {
        let name = pos_name(&c.qualifier, env)?;
        Some(name)
    };

    match c.qualifier.kind() {
        SyntaxKind::Missing => {}
        SyntaxKind::QualifiedName => {}
        SyntaxKind::Token(TK::Name) => {}
        _ => raise_parsing_error(
            &c.qualifier,
            env,
            &syntax_error::invalid_enum_class_label_qualifier,
        ),
    };
    Ok(Expr_::mk_enum_class_label(qual, label_name))
}

fn p_package_expr<'a>(
    p: &'a PackageExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let id = pos_name(&p.name, env)?;
    Ok(Expr_::mk_package(id))
}

fn p_nameof<'a>(
    p: &'a NameofExpressionChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    let basic_error = |env| Error::ParsingError {
        message: String::from("`nameof` can only be used with a class"),
        pos: p_pos(&p.target, env),
    };
    match &p.target.children {
        Token(_) | SimpleTypeSpecifier(_) | QualifiedName(_) => {
            let target = p_expr(&p.target, env)?;
            match &target.2 {
                Expr_::Id(id) if special_typehints::STRING == id.name() => {
                    Err(Error::ParsingError {
                        message: String::from("`nameof` cannot be used with string"),
                        pos: p_pos(&p.target, env),
                    })
                }
                Expr_::Id(id) => {
                    if env.get_reification(id.name()).is_some() {
                        Err(Error::ParsingError {
                            message: String::from("`nameof` cannot be used with a generic type"),
                            pos: p_pos(&p.target, env),
                        })
                    } else if string_utils::is_self(id.name()) {
                        Ok(Expr_::mk_nameof(ast::ClassId(
                            (),
                            target.1.clone(),
                            ast::ClassId_::CIself,
                        )))
                    } else if string_utils::is_parent(id.name()) {
                        Ok(Expr_::mk_nameof(ast::ClassId(
                            (),
                            target.1.clone(),
                            ast::ClassId_::CIparent,
                        )))
                    } else {
                        Ok(Expr_::mk_nameof(ast::ClassId(
                            (),
                            target.1.clone(),
                            ast::ClassId_::CIexpr(target),
                        )))
                    }
                }
                _ => Err(basic_error(env)),
            }
        }
        _ => Err(basic_error(env)),
    }
}

fn mk_lid(p: Pos, s: String) -> ast::Lid {
    ast::Lid(p, (0, s))
}

fn mk_name_lid<'a>(name: S<'a>, env: &mut Env<'a>) -> Result<ast::Lid> {
    let name = pos_name(name, env)?;
    Ok(mk_lid(name.0, name.1))
}

fn mk_lvar<'a>(name: S<'a>, env: &mut Env<'a>) -> Result<Expr_> {
    Ok(Expr_::mk_lvar(mk_name_lid(name, env)?))
}

fn mk_id_expr(name: ast::Sid) -> ast::Expr {
    ast::Expr::new((), name.0.clone(), Expr_::mk_id(name))
}

fn p_special_call<'a>(recv: S<'a>, args: S<'a>, e: &mut Env<'a>) -> Result<Expr_> {
    // Mark expression as CallReceiver so that we can correctly set
    // PropOrMethod field in ObjGet and ClassGet
    let recv = p_expr_with_loc(ExprLocation::CallReceiver, recv, e, None)?;
    let (args, unpacked_arg) = split_args_unpacked_arg(args, e)?;
    Ok(Expr_::mk_call(ast::CallExpr {
        func: recv,
        targs: vec![],
        args,
        unpacked_arg,
    }))
}

fn p_obj_get<'a>(
    location: ExprLocation,
    recv: S<'a>,
    op: S<'a>,
    name: S<'a>,
    e: &mut Env<'a>,
) -> Result<Expr_> {
    if recv.is_object_creation_expression() && !e.is_codegen() {
        raise_parsing_error(recv, e, &syntax_error::invalid_constructor_method_call);
    }
    let recv = p_expr(recv, e)?;
    let name = p_expr_with_loc(ExprLocation::MemberSelect, name, e, None)?;
    let op = p_null_flavor(op, e)?;
    Ok(Expr_::mk_obj_get(
        recv,
        name,
        op,
        match location {
            ExprLocation::CallReceiver => ast::PropOrMethod::IsMethod,
            _ => ast::PropOrMethod::IsProp,
        },
    ))
}

fn p_xhp_embedded<'a, F>(node: S<'a>, env: &mut Env<'a>, escaper: F) -> Result<ast::Expr>
where
    F: FnOnce(&[u8]) -> Vec<u8>,
{
    if let Some(kind) = token_kind(node) {
        if env.is_codegen() && TK::XHPStringLiteral == kind {
            let p = p_pos(node, env);
            /* for XHP string literals (attribute values) just extract
            value from quotes and decode HTML entities  */
            let text = html_entities::decode(get_quoted_content(node.full_text(env.source_text())));
            Ok(ast::Expr::new((), p, Expr_::make_string(text)))
        } else if env.is_codegen() && TK::XHPBody == kind {
            let p = p_pos(node, env);
            /* for XHP body - only decode HTML entities */
            let text = html_entities::decode(&unesc_xhp(node.full_text(env.source_text())));
            Ok(ast::Expr::new((), p, Expr_::make_string(text)))
        } else {
            let p = p_pos(node, env);
            let s = escaper(node.full_text(env.source_text()));
            Ok(ast::Expr::new((), p, Expr_::make_string(s)))
        }
    } else {
        p_expr(node, env)
    }
}

fn p_xhp_attr<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::XhpAttribute> {
    match &node.children {
        XHPSimpleAttribute(c) => {
            let attr_expr = &c.expression;
            let name = p_pstring(&c.name, env)?;
            let expr = if attr_expr.is_braced_expression()
                && env.file_mode() == file_info::Mode::Mhhi
                && !env.is_codegen()
            {
                ast::Expr::new((), env.mk_none_pos(), Expr_::Null)
            } else {
                p_xhp_embedded(attr_expr, env, unesc_xhp_attr)?
            };
            let xhp_simple = ast::XhpSimple {
                name,
                type_: (),
                expr,
            };
            Ok(ast::XhpAttribute::XhpSimple(xhp_simple))
        }
        XHPSpreadAttribute(c) => {
            let expr = p_xhp_embedded(&c.expression, env, unesc_xhp)?;
            Ok(ast::XhpAttribute::XhpSpread(expr))
        }
        _ => missing_syntax("XHP attribute", node, env),
    }
}

fn aggregate_xhp_tokens<'a>(env: &mut Env<'a>, nodes: S<'a>) -> Result<Vec<S<'a>>> {
    let nodes = nodes.syntax_node_to_list_skip_separator();
    let mut state = (None, None, vec![]); // (start, end, result)
    let mut combine = |state: &mut (Option<S<'a>>, Option<S<'a>>, Vec<S<'a>>)| {
        match (state.0, state.1) {
            (Some(s), None) => state.2.push(s),
            (Some(s), Some(e)) => {
                let token = env
                    .token_factory
                    .concatenate(s.get_token().unwrap(), e.get_token().unwrap());
                let node = env.arena.alloc(Syntax::make_token(token));
                state.2.push(node)
            }
            _ => {}
        }
        state.0 = None;
        state.1 = None;
        Ok(())
    };
    for n in nodes {
        match &n.children {
            Token(t) if t.kind() == TK::XHPComment => {
                if state.0.is_some() {
                    combine(&mut state)?;
                }
            }
            Token(_) => {
                if state.0.is_none() {
                    state.0 = Some(n)
                } else {
                    state.1 = Some(n)
                }
            }
            _ => {
                combine(&mut state)?;
                state.2.push(n);
            }
        }
    }
    combine(&mut state)?;
    Ok(state.2)
}

fn p_bop<'a>(
    pos: Pos,
    node: S<'a>,
    lhs: ast::Expr,
    rhs: ast::Expr,
    env: &mut Env<'a>,
) -> Result<Expr_> {
    use ast::Bop::*;
    let mk = |bop, lhs, rhs| Ok(Expr_::mk_binop(Binop { bop, lhs, rhs }));
    let mk_eq = |op, lhs, rhs| Ok(Expr_::mk_assign(lhs, Some(op), rhs));
    match token_kind(node) {
        Some(TK::Equal) => Ok(Expr_::mk_assign(lhs, None, rhs)),
        Some(TK::Bar) => mk(Bar, lhs, rhs),
        Some(TK::Ampersand) => mk(Amp, lhs, rhs),
        Some(TK::Plus) => mk(Plus, lhs, rhs),
        Some(TK::Minus) => mk(Minus, lhs, rhs),
        Some(TK::Star) => mk(Star, lhs, rhs),
        Some(TK::Carat) => mk(Xor, lhs, rhs),
        Some(TK::Slash) => mk(Slash, lhs, rhs),
        Some(TK::Dot) => mk(Dot, lhs, rhs),
        Some(TK::Percent) => mk(Percent, lhs, rhs),
        Some(TK::LessThan) => mk(Lt, lhs, rhs),
        Some(TK::GreaterThan) => mk(Gt, lhs, rhs),
        Some(TK::EqualEqual) => mk(Eqeq, lhs, rhs),
        Some(TK::LessThanEqual) => mk(Lte, lhs, rhs),
        Some(TK::GreaterThanEqual) => mk(Gte, lhs, rhs),
        Some(TK::StarStar) => mk(Starstar, lhs, rhs),
        Some(TK::ExclamationEqual) => mk(Diff, lhs, rhs),
        Some(TK::BarEqual) => mk_eq(Bar, lhs, rhs),
        Some(TK::PlusEqual) => mk_eq(Plus, lhs, rhs),
        Some(TK::MinusEqual) => mk_eq(Minus, lhs, rhs),
        Some(TK::StarEqual) => mk_eq(Star, lhs, rhs),
        Some(TK::StarStarEqual) => mk_eq(Starstar, lhs, rhs),
        Some(TK::SlashEqual) => mk_eq(Slash, lhs, rhs),
        Some(TK::DotEqual) => mk_eq(Dot, lhs, rhs),
        Some(TK::PercentEqual) => mk_eq(Percent, lhs, rhs),
        Some(TK::CaratEqual) => mk_eq(Xor, lhs, rhs),
        Some(TK::AmpersandEqual) => mk_eq(Amp, lhs, rhs),
        Some(TK::BarBar) => mk(Barbar, lhs, rhs),
        Some(TK::AmpersandAmpersand) => mk(Ampamp, lhs, rhs),
        Some(TK::LessThanLessThan) => mk(Ltlt, lhs, rhs),
        Some(TK::GreaterThanGreaterThan) => mk(Gtgt, lhs, rhs),
        Some(TK::EqualEqualEqual) => mk(Eqeqeq, lhs, rhs),
        Some(TK::LessThanLessThanEqual) => mk_eq(Ltlt, lhs, rhs),
        Some(TK::GreaterThanGreaterThanEqual) => mk_eq(Gtgt, lhs, rhs),
        Some(TK::ExclamationEqualEqual) => mk(Diff2, lhs, rhs),
        Some(TK::LessThanEqualGreaterThan) => mk(Cmp, lhs, rhs),
        Some(TK::QuestionQuestion) => mk(QuestionQuestion, lhs, rhs),
        Some(TK::QuestionQuestionEqual) => mk_eq(QuestionQuestion, lhs, rhs),
        /* The ugly duckling; In the FFP, `|>` is parsed as a
         * `BinaryOperator`, whereas the typed AST has separate constructors for
         * Pipe and Binop. This is why we don't just project onto a
         * `bop`, but a `expr -> expr -> expr_`.
         */
        Some(TK::BarGreaterThan) => {
            let lid =
                ast::Lid::from_counter(pos, env.next_local_id(), special_idents::DOLLAR_DOLLAR);
            Ok(Expr_::mk_pipe(
                lid,
                lhs,
                rhs,
                ast::OperatorNullFlavor::Regular,
            ))
        }
        Some(TK::BarQuestionGreaterThan) => {
            let lid =
                ast::Lid::from_counter(pos, env.next_local_id(), special_idents::DOLLAR_DOLLAR);
            Ok(Expr_::mk_pipe(
                lid,
                lhs,
                rhs,
                ast::OperatorNullFlavor::Nullsafe,
            ))
        }
        Some(TK::QuestionColon) => Ok(Expr_::mk_eif(lhs, None, rhs)),
        _ => missing_syntax("binary operator", node, env),
    }
}

fn p_exprs_with_loc<'a>(n: S<'a>, e: &mut Env<'a>) -> Result<(Pos, Vec<ast::Expr>)> {
    Ok((
        p_pos(n, e),
        could_map(n, e, |n, e| {
            p_expr_with_loc(ExprLocation::UsingStatement, n, e, None)
        })?,
    ))
}

fn p_stmt_list_<'a>(
    pos: &Pos,
    mut nodes: Iter<'_, S<'a>>,
    env: &mut Env<'a>,
) -> Result<Vec<ast::Stmt>> {
    let mut r = vec![];
    loop {
        match nodes.next() {
            Some(n) => match &n.children {
                UsingStatementFunctionScoped(c) => {
                    let body = p_stmt_list_(pos, nodes, env)?;
                    let has_await = !c.await_keyword.is_missing();
                    env.found_await |= has_await;
                    let using = ast::Stmt::new(
                        pos.clone(),
                        ast::Stmt_::mk_using(ast::UsingStmt {
                            is_block_scoped: false,
                            has_await,
                            exprs: p_exprs_with_loc(&c.expression, env)?,
                            block: ast::Block(body),
                        }),
                    );
                    r.push(using);
                    break Ok(r);
                }
                _ => {
                    r.push(p_stmt(n, env)?);
                }
            },
            _ => break Ok(r),
        }
    }
}

fn handle_loop_body<'a>(pos: Pos, node: S<'a>, env: &mut Env<'a>) -> Result<ast::Stmt> {
    let list: Vec<_> = node.syntax_node_to_list_skip_separator().collect();
    let blk: ast::Block = p_stmt_list_(&pos, list.iter(), env)?
        .into_iter()
        .filter(|stmt| !stmt.1.is_noop())
        .collect();
    let body = if blk.is_empty() {
        ast::Block(vec![mk_noop(env)])
    } else {
        blk
    };
    Ok(ast::Stmt::new(pos, ast::Stmt_::mk_block(None, body)))
}

fn p_stmt<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Stmt> {
    let docblock = extract_docblock(node, env);
    env.push_docblock(docblock);
    let result = p_stmt_(node, env);
    env.pop_docblock();
    result
}

fn p_stmt_<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Stmt> {
    let pos = p_pos(node, env);
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    match &node.children {
        SwitchStatement(c) => p_switch_stmt_(env, pos, c),
        MatchStatement(c) => p_match_stmt(env, pos, c),
        IfStatement(c) => p_if_stmt(env, pos, c),
        ExpressionStatement(c) => p_expression_stmt(env, pos, c),
        CompoundStatement(c) => handle_loop_body(pos, &c.statements, env),
        SyntaxList(_) => handle_loop_body(pos, node, env),
        ThrowStatement(c) => p_throw_stmt(env, pos, c),
        DoStatement(c) => p_do_stmt(env, pos, c),
        WhileStatement(c) => p_while_stmt(env, pos, c),
        UsingStatementBlockScoped(c) => p_using_statement_block_scoped_stmt(env, pos, c),
        UsingStatementFunctionScoped(c) => p_using_statement_function_scoped_stmt(env, pos, c),
        ForStatement(c) => p_for_stmt(env, pos, c),
        ForeachStatement(c) => p_foreach_stmt(env, pos, c),
        TryStatement(c) => p_try_stmt(env, pos, c),
        ReturnStatement(c) => p_return_stmt(env, pos, c),
        YieldBreakStatement(_) => {
            env.found_yield = true;
            Ok(ast::Stmt::new(pos, ast::Stmt_::mk_yield_break()))
        }
        EchoStatement(c) => p_echo_stmt(env, pos, c),
        UnsetStatement(c) => p_unset_stmt(env, pos, c),
        BreakStatement(_) => Ok(new(pos, S_::Break)),
        ContinueStatement(_) => Ok(new(pos, S_::Continue)),
        ConcurrentStatement(c) => p_concurrent_stmt(env, pos, c),
        MarkupSection(_) => p_markup(node, env),
        _ => {
            raise_missing_syntax("statement", node, env);
            Ok(new(env.mk_none_pos(), S_::Noop))
        }
    }
}

fn p_loop_cond<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::LoopCond> {
    Ok(ast::LoopCond(None, None, p_expr(node, env)?))
}

fn p_loop_iter<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::LoopIter> {
    Ok(ast::LoopIter(None, None, could_map(node, env, p_expr)?))
}

fn p_while_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a WhileStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    Ok(new(
        pos,
        S_::mk_while(
            p_loop_cond(&c.condition, env)?,
            p_block(true, &c.body, env)?,
        ),
    ))
}

fn p_throw_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ThrowStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    Ok(new(pos, S_::mk_throw(p_expr(&c.expression, env)?)))
}

fn check_continue_break<'a>(env: &mut Env<'a>, blk_kind: TK, blk: &ast::Block) {
    blk.iter().for_each(|ast::Stmt(p_, stmt_)| match stmt_ {
        ast::Stmt_::Continue => raise_parsing_error_pos(
            p_,
            env,
            &syntax_error::no_continue_in_finally(blk_kind, TK::Continue),
        ),
        ast::Stmt_::Break => raise_parsing_error_pos(
            p_,
            env,
            &syntax_error::no_continue_in_finally(blk_kind, TK::Break),
        ),
        _ => (),
    });
}

fn p_try_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a TryStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let try_blk = p_block(false, &c.compound_statement, env)?;
    Ok(new(
        pos,
        S_::mk_try(
            try_blk,
            could_map(&c.catch_clauses, env, |n, e| match &n.children {
                CatchClause(c) => Ok(ast::Catch(
                    pos_name(&c.type_, e)?,
                    lid_from_name(&c.variable, e)?,
                    p_block(true, &c.body, e)?,
                )),
                _ => missing_syntax("catch clause", n, e),
            })?,
            match &c.finally_clause.children {
                FinallyClause(c) => p_finally_block(false, &c.body, env)?,
                _ => Default::default(),
            },
        ),
    ))
}

fn p_concurrent_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ConcurrentStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    if env.parser_options.unwrap_concurrent {
        return p_stmt(&c.statement, env);
    }
    Ok(new(
        match env.codegen {
            Mode::ForCodegen =>
            // Existing uses and tests have different expectations for the location depending
            // on whether it's for codegen or not
            {
                p_pos(&c.keyword, env)
            }
            Mode::ForTypecheck => pos,
        },
        S_::mk_concurrent(p_block(true, &c.statement, env)?),
    ))
}

fn p_unset_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a UnsetStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let args = could_map(&c.variables, env, p_expr_for_normal_argument)?;
    let unset = match &c.keyword.children {
        QualifiedName(_) | SimpleTypeSpecifier(_) | Token(_) => {
            let name = pos_name(&c.keyword, env)?;
            ast::Expr::new((), name.0.clone(), Expr_::mk_id(name))
        }
        _ => missing_syntax("id", &c.keyword, env)?,
    };
    Ok(new(
        pos.clone(),
        S_::mk_expr(ast::Expr::new(
            (),
            pos,
            Expr_::mk_call(ast::CallExpr {
                func: unset,
                targs: vec![],
                args,
                unpacked_arg: None,
            }),
        )),
    ))
}

fn p_echo_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a EchoStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let echo = match &c.keyword.children {
        QualifiedName(_) | SimpleTypeSpecifier(_) | Token(_) => {
            let name = pos_name(&c.keyword, env)?;
            ast::Expr::new((), name.0.clone(), Expr_::mk_id(name))
        }
        _ => missing_syntax("id", &c.keyword, env)?,
    };
    let args = could_map(&c.expressions, env, p_expr_for_normal_argument)?;
    Ok(new(
        pos.clone(),
        S_::mk_expr(ast::Expr::new(
            (),
            pos,
            Expr_::mk_call(ast::CallExpr {
                func: echo,
                targs: vec![],
                args,
                unpacked_arg: None,
            }),
        )),
    ))
}

fn p_return_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ReturnStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    let expr = match &c.expression.children {
        Missing => None,
        _ => Some(p_expr_with_loc(
            ExprLocation::RightOfReturn,
            &c.expression,
            env,
            None,
        )?),
    };
    Ok(ast::Stmt::new(pos, ast::Stmt_::mk_return(expr)))
}

fn p_foreach_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ForeachStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let col = p_expr(&c.collection, env)?;
    let akw = match token_kind(&c.await_keyword) {
        Some(TK::Await) => {
            env.found_await = true;
            Some(p_pos(&c.await_keyword, env))
        }
        _ => None,
    };
    let value = p_expr_with_loc(ExprLocation::LvaluePosition, &c.value, env, None)?;
    // The key position also uses LvaluePosition so destructuring works on both
    // sides of `foreach ($xs as key => value)`.
    let akv = match (akw, &c.key.children) {
        (Some(p), Missing) => ast::AsExpr::AwaitAsV(p, value),
        (None, Missing) => ast::AsExpr::AsV(value),
        (Some(p), _) => ast::AsExpr::AwaitAsKv(
            p,
            p_expr_with_loc(ExprLocation::LvaluePosition, &c.key, env, None)?,
            value,
        ),
        (None, _) => ast::AsExpr::AsKv(
            p_expr_with_loc(ExprLocation::LvaluePosition, &c.key, env, None)?,
            value,
        ),
    };
    let blk = p_block(true, &c.body, env)?;
    Ok(new(pos, S_::mk_foreach(col, akv, blk)))
}

fn p_for_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ForStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;

    let ini = could_map(&c.initializer, env, p_expr)?;
    let ctr = map_optional(&c.control, env, p_loop_cond)?;
    let eol = p_loop_iter(&c.end_of_loop, env)?;
    let blk = p_block(true, &c.body, env)?;
    Ok(Stmt::new(pos, S_::mk_for(ini, ctr, eol, blk)))
}

fn p_using_statement_function_scoped_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a UsingStatementFunctionScopedChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    let has_await = !&c.await_keyword.is_missing();
    env.found_await |= has_await;
    Ok(new(
        pos,
        S_::mk_using(ast::UsingStmt {
            is_block_scoped: false,
            has_await,
            exprs: p_exprs_with_loc(&c.expression, env)?,
            block: ast::Block(vec![mk_noop(env)]),
        }),
    ))
}

fn p_using_statement_block_scoped_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a UsingStatementBlockScopedChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;
    let has_await = !&c.await_keyword.is_missing();
    env.found_await |= has_await;
    Ok(new(
        pos,
        S_::mk_using(ast::UsingStmt {
            is_block_scoped: true,
            has_await,
            exprs: p_exprs_with_loc(&c.expressions, env)?,
            block: p_block(false, &c.body, env)?,
        }),
    ))
}

fn p_do_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a DoStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    Ok(new(
        pos,
        S_::mk_do(
            p_block(false /* remove noop */, &c.body, env)?,
            p_loop_cond(&c.condition, env)?,
        ),
    ))
}

fn p_expression_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a ExpressionStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let expr = &c.expression;
    if expr.is_missing() {
        Ok(new(pos, S_::Noop))
    } else {
        Ok(new(
            pos,
            S_::mk_expr(p_expr_with_loc(ExprLocation::AsStatement, expr, env, None)?),
        ))
    }
}

fn p_if_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a IfStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let condition = p_expr(&c.condition, env)?;
    let statement = p_block(true /* remove noop */, &c.statement, env)?;
    let else_ = match &c.else_clause.children {
        ElseClause(c) => p_block(true, &c.statement, env)?,
        Missing => ast::Block(vec![mk_noop(env)]),
        _ => missing_syntax("else clause", &c.else_clause, env)?,
    };
    Ok(new(pos, S_::mk_if(condition, statement, else_)))
}

fn p_switch_stmt_<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a SwitchStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    use ast::Stmt;
    use ast::Stmt_ as S_;
    let new = Stmt::new;

    let p_label = |n: S<'a>, e: &mut Env<'a>| -> Result<ast::GenCase> {
        match &n.children {
            CaseLabel(c) => Ok(aast::GenCase::Case(aast::Case(
                p_expr(&c.expression, e)?,
                // inject a fallthrough for all not-last labels
                // (last ones get overwritten in p_section)
                {
                    let mut blk = ast::Block::default();
                    blk.push(new(e.mk_none_pos(), S_::Fallthrough));
                    blk
                },
            ))),
            DefaultLabel(_) => Ok(aast::GenCase::Default(aast::DefaultCase(
                p_pos(n, e),
                Default::default(),
            ))),
            _ => missing_syntax("switch label", n, e),
        }
    };
    let p_section = |n: S<'a>, e: &mut Env<'a>| -> Result<Vec<ast::GenCase>> {
        match &n.children {
            SwitchSection(c) => {
                let mut blk = ast::Block(could_map(&c.statements, e, p_stmt)?);
                if !c.fallthrough.is_missing() {
                    blk.push(new(e.mk_none_pos(), S_::Fallthrough));
                }
                let mut labels = could_map(&c.labels, e, p_label)?;
                match labels.last_mut() {
                    Some(aast::GenCase::Default(aast::DefaultCase(_, b))) => *b = blk,
                    Some(aast::GenCase::Case(aast::Case(_, b))) => *b = blk,
                    _ => raise_parsing_error(n, e, "Malformed block result"),
                }
                Ok(labels)
            }
            _ => missing_syntax("switch section", n, e),
        }
    };

    let cases = itertools::concat(could_map(&c.sections, env, p_section)?);

    let last_is_default = matches!(cases.last(), Some(aast::GenCase::Default(_)));

    let (cases, mut defaults): (Vec<ast::Case>, Vec<ast::DefaultCase>) =
        cases.into_iter().partition_map(|case| match case {
            aast::GenCase::Case(x @ aast::Case(..)) => Either::Left(x),
            aast::GenCase::Default(x @ aast::DefaultCase(..)) => Either::Right(x),
        });

    if defaults.len() > 1 {
        let aast::DefaultCase(pos, _) = &defaults[1];
        raise_parsing_error_pos(pos, env, &syntax_error::multiple_defaults_in_switch);
    }

    let default = match defaults.pop() {
        Some(default @ aast::DefaultCase(..)) => {
            if last_is_default {
                Some(default)
            } else {
                let aast::DefaultCase(pos, _) = default;
                raise_parsing_error_pos(&pos, env, &syntax_error::default_switch_case_not_last);
                None
            }
        }

        None => None,
    };

    Ok(new(
        pos,
        S_::mk_switch(p_expr(&c.expression, env)?, cases, default),
    ))
}

fn p_match_stmt<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a MatchStatementChildren<'a, PositionedToken<'a>, PositionedValue<'a>>,
) -> Result<ast::Stmt> {
    let expr = p_expr(&c.expression, env)?;
    let arms = could_map(&c.arms, env, p_match_stmt_arm)?;
    Ok(ast::Stmt::new(
        pos,
        ast::Stmt_::Match(Box::new(ast::StmtMatch { expr, arms })),
    ))
}

fn p_match_stmt_arm<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::StmtMatchArm> {
    let c = match &node.children {
        MatchStatementArm(c) => c,
        _ => return missing_syntax("match statement", node, env),
    };
    let pat = p_pat(&c.pattern, env)?;
    let body = p_block(true /* remove noop */, &c.body, env)?;
    Ok(ast::StmtMatchArm { pat, body })
}

fn p_pat<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Pattern> {
    let pos = p_pos(node, env);
    match &node.children {
        VariablePattern(c) => p_variable_pat(env, pos, c),
        RefinementPattern(c) => p_refinement_pat(env, pos, c),
        ConstructorPattern(c) => {
            // Constructor patterns are not yet supported. Wildcard patterns
            // are, and they're represented in the CST as constructor patterns
            // with no members.
            if !c.members.is_missing() {
                raise_parsing_error(node, env, &syntax_error::destructuring_patterns_nyi);
            }
            if token_kind(&c.constructor) == Some(TK::Name) {
                let name = c.constructor.text(env.source_text());
                if !name.starts_with('_') {
                    raise_parsing_error_pos(&pos, env, &syntax_error::wildcard_underscore(name));
                }
            } else {
                raise_parsing_error(node, env, &syntax_error::constructor_patterns_nyi);
            }
            Ok(ast::Pattern::PVar(Box::new(ast::PatVar { pos, id: None })))
        }
        _ => missing_syntax("pattern", node, env),
    }
}

fn p_variable_or_wildcard<'a>(
    pos: Pos,
    name: S<'a>,
    env: &mut Env<'a>,
) -> Result<Option<ast::Lid>> {
    match token_kind(name) {
        Some(TK::Variable) => {
            raise_parsing_error_pos(&pos, env, &syntax_error::variable_patterns_nyi);
            Ok(Some(lid_from_pos_name(pos, name, env)?))
        }
        Some(TK::Name) => {
            let name = name.text(env.source_text());
            if !name.starts_with('_') {
                raise_parsing_error_pos(&pos, env, &syntax_error::wildcard_underscore(name));
            }
            Ok(None)
        }
        _ => missing_syntax("variable or wildcard", name, env),
    }
}

fn p_variable_pat<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a VariablePatternChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
) -> Result<ast::Pattern> {
    Ok(ast::Pattern::PVar(Box::new(ast::PatVar {
        pos: pos.clone(),
        id: p_variable_or_wildcard(pos, &c.variable, env)?,
    })))
}

fn p_refinement_pat<'a>(
    env: &mut Env<'a>,
    pos: Pos,
    c: &'a RefinementPatternChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
) -> Result<ast::Pattern> {
    Ok(ast::Pattern::PRefinement(Box::new(ast::PatRefinement {
        pos: pos.clone(),
        id: p_variable_or_wildcard(pos, &c.variable, env)?,
        hint: p_hint(&c.specifier, env)?,
    })))
}

fn p_markup<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Stmt> {
    match &node.children {
        MarkupSection(c) => {
            let markup_hashbang = &c.hashbang;
            let markup_suffix = &c.suffix;
            let pos = p_pos(node, env);
            let f = pos.filename();
            let expected_suffix_offset = if markup_hashbang.is_missing() {
                0
            } else {
                markup_hashbang.width() + 1 /* for newline */
            };
            if (f.has_extension("hack") || f.has_extension("hackpartial"))
                && !(markup_suffix.is_missing())
            {
                let ext = f.path().extension().unwrap(); // has_extension ensures this is a Some
                raise_parsing_error(node, env, &syntax_error::error1060(ext.to_str().unwrap()));
            } else if f.has_extension("php")
                && !markup_suffix.is_missing()
                && markup_suffix.offset() != Some(expected_suffix_offset)
            {
                raise_parsing_error(markup_suffix, env, &syntax_error::error1001);
            }
            // Check that the name after <? is either "hh" or "php"
            if let MarkupSuffix(suffix_children) = &markup_suffix.children {
                let name = &suffix_children.name;
                if !name.is_missing() {
                    let name_text = text_str(name, env);
                    if !name_text.eq_ignore_ascii_case("hh")
                        && !name_text.eq_ignore_ascii_case("php")
                    {
                        raise_parsing_error(name, env, &syntax_error::error1061(name_text));
                    }
                }
            }
            let stmt_ = ast::Stmt_::mk_markup((pos.clone(), text(markup_hashbang, env)));
            Ok(ast::Stmt::new(pos, stmt_))
        }
        _ => missing_syntax("XHP markup node", node, env),
    }
}

fn p_modifiers<'a, F: Fn(R, modifier::Kind) -> R, R>(
    on_kind: F,
    mut init: R,
    node: S<'a>,
    env: &mut Env<'a>,
) -> (modifier::KindSet, R) {
    let mut kind_set = modifier::KindSet::new();
    for n in node.syntax_node_to_list_skip_separator() {
        let token_kind = token_kind(n).and_then(modifier::from_token_kind);
        match token_kind {
            Some(kind) => {
                kind_set.add(kind);
                init = on_kind(init, kind);
            }
            _ => {
                raise_missing_syntax("kind", n, env);
            }
        }
    }
    if kind_set.has(modifier::PROTECTED) && kind_set.has(modifier::INTERNAL) {
        init = on_kind(init, modifier::PROTECTED_INTERNAL);
    }
    (kind_set, init)
}

fn p_kinds<'a>(node: S<'a>, env: &mut Env<'a>) -> modifier::KindSet {
    p_modifiers(|_, _| {}, (), node, env).0
}

/// Apply `f` to every item in `node`, and build a vec of the values returned.
fn could_map<'a, R, F>(node: S<'a>, env: &mut Env<'a>, f: F) -> Result<Vec<R>>
where
    F: Fn(S<'a>, &mut Env<'a>) -> Result<R>,
{
    let nodes = node.syntax_node_to_list_skip_separator();
    let (min, _) = nodes.size_hint();
    let mut v = Vec::with_capacity(min);
    for n in nodes {
        v.push(f(n, env)?);
    }
    Ok(v)
}

fn could_map_emit_error<'a, R, F>(node: S<'a>, env: &mut Env<'a>, f: F) -> Vec<R>
where
    F: Fn(S<'a>, &mut Env<'a>) -> Result<R>,
{
    let mut v = vec![];
    for n in node.syntax_node_to_list_skip_separator() {
        match f(n, env) {
            Ok(value) => {
                v.push(value);
            }
            Err(e) => {
                emit_error(e, env);
            }
        }
    }

    v
}

fn p_visibility<'a>(node: S<'a>, env: &mut Env<'a>) -> Option<ast::Visibility> {
    let first_vis = |r: Option<ast::Visibility>, kind| r.or_else(|| modifier::to_visibility(kind));
    p_modifiers(first_vis, None, node, env).1
}

fn p_visibility_last_win<'a>(node: S<'a>, env: &mut Env<'a>) -> Option<ast::Visibility> {
    let last_vis = |r, kind| modifier::to_visibility(kind).or(r);
    p_modifiers(last_vis, None, node, env).1
}

fn p_visibility_last_win_or<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
    default: ast::Visibility,
) -> ast::Visibility {
    p_visibility_last_win(node, env).unwrap_or(default)
}

fn has_soft(attrs: &[ast::UserAttribute]) -> bool {
    attrs.iter().any(|attr| attr.name.1 == special_attrs::SOFT)
}

fn soften_hint(attrs: &[ast::UserAttribute], hint: ast::Hint) -> ast::Hint {
    if has_soft(attrs) {
        ast::Hint::new(hint.0.clone(), ast::Hint_::Hsoft(hint))
    } else {
        hint
    }
}

fn strip_ns(name: &str) -> &str {
    match name.chars().next() {
        Some('\\') => &name[1..],
        _ => name,
    }
}

// The contexts `ctx $f`, `$a::C`, and `T::C` all depend on the ability to generate a backing tparam
// on the function or method. The `this::C` context does not, so it may appear in more places.
fn is_polymorphic_context<'a>(env: &mut Env<'a>, hint: &ast::Hint, ignore_this: bool) -> bool {
    use ast::Hint_::Haccess;
    use ast::Hint_::Happly;
    use ast::Hint_::HfunContext;
    use ast::Hint_::Hvar;
    match *hint.1 {
        HfunContext(_) => true,
        Haccess(ref root, _) => match &*root.1 {
            Happly(oxidized::ast::Id(_, id), _) => {
                let s = id.as_str();
                /* TODO(coeffects) There is an opportunity to represent this structurally
                 * in the AST if we refactor so generic hints lower as Habstr instead of
                 * Happly, like we do in the direct decl parser. */
                (strip_ns(s) == sn::typehints::THIS && !ignore_this)
                    || env.fn_generics_mut().contains_key(s)
                    || env.cls_generics_mut().contains_key(s)
            }
            Hvar(_) => true,
            _ => false,
        },
        _ => false,
    }
}

fn has_polymorphic_context<'a>(env: &mut Env<'a>, contexts: Option<&ast::Contexts>) -> bool {
    if let Some(ast::Contexts(_, context_hints)) = contexts {
        context_hints
            .iter()
            .any(|c| is_polymorphic_context(env, c, false))
    } else {
        false
    }
}

fn has_any_policied_context(contexts: Option<&ast::Contexts>) -> bool {
    if let Some(ast::Contexts(_, context_hints)) = contexts {
        context_hints.iter().any(|hint| match &*hint.1 {
            ast::Hint_::Happly(ast::Id(_, id), _) => sn::coeffects::is_any_zoned(id),
            _ => false,
        })
    } else {
        false
    }
}

fn has_any_policied_or_defaults_context(contexts: Option<&ast::Contexts>) -> bool {
    if let Some(ast::Contexts(_, context_hints)) = contexts {
        context_hints.iter().any(|hint| match &*hint.1 {
            ast::Hint_::Happly(ast::Id(_, id), _) => sn::coeffects::is_any_zoned_or_defaults(id),
            _ => false,
        })
    } else {
        true
    }
}

fn contexts_cannot_access_ic(haystack: Option<&ast::Contexts>) -> bool {
    if let Some(ast::Contexts(_, context_hints)) = haystack {
        context_hints.iter().all(|hint| match &*hint.1 {
            ast::Hint_::Happly(ast::Id(_, id), _) => {
                sn::coeffects::is_any_without_implicit_policy_or_unsafe(id)
            }
            _ => false,
        })
    } else {
        false // no context list -> implicit [defaults]
    }
}

// For polymorphic context with form `ctx $f`
// require that `(function (ts)[_]: t) $f` exists
// rewrite as `(function (ts)[ctx $f]: t) $f`
// add a type parameter named "T/[ctx $f]"
fn rewrite_fun_ctx<'a>(
    env: &mut Env<'a>,
    tparams: &mut Vec<ast::Tparam>,
    hint: &mut ast::Hint,
    name: &str,
) {
    use ast::Hint_;
    use ast::ReifyKind;
    use ast::Variance;

    let mut invalid =
        |p| raise_parsing_error_pos(p, env, &syntax_error::ctx_fun_invalid_type_hint(name));
    match *hint.1 {
        Hint_::Hfun(ref mut hf) => {
            if let &mut Some(ast::Contexts(ref p, ref mut hl)) = &mut hf.ctxs {
                if let [ref mut h] = *hl.as_mut_slice() {
                    if let Hint_::Hwildcard = &*h.1 {
                        *h.1 = Hint_::HfunContext(name.to_string());
                        tparams.push(ast::Tparam {
                            variance: Variance::Invariant,
                            name: ast::Id(h.0.clone(), format!("T/[ctx {}]", name)),
                            constraints: vec![],
                            reified: ReifyKind::Erased,
                            user_attributes: Default::default(),
                        });
                    } else {
                        invalid(&h.0);
                    }
                } else {
                    invalid(p);
                }
            } else {
                invalid(&hint.0);
            }
        }
        Hint_::Hlike(ref mut h) | Hint_::Hoption(ref mut h) => {
            rewrite_fun_ctx(env, tparams, h, name)
        }
        Hint_::Happly(ast::Id(_, ref type_name), ref mut targs)
            if type_name == special_typehints::SUPPORTDYN =>
        {
            if let Some(ref mut h) = targs.first_mut() {
                rewrite_fun_ctx(env, tparams, h, name)
            } else {
                invalid(&hint.0)
            }
        }
        _ => invalid(&hint.0),
    }
}
fn rewrite_effect_polymorphism<'a>(
    env: &mut Env<'a>,
    params: &mut [ast::FunParam],
    tparams: &mut Vec<ast::Tparam>,
    contexts: Option<&ast::Contexts>,
    where_constraints: &mut Vec<ast::WhereConstraintHint>,
) {
    use Hint_::Haccess;
    use Hint_::Happly;
    use Hint_::HfunContext;
    use Hint_::Hvar;
    use ast::Hint;
    use ast::Hint_;
    use ast::ReifyKind;
    use ast::Variance;

    if !has_polymorphic_context(env, contexts) {
        return;
    }
    let ast::Contexts(_p, context_hints) = contexts.as_ref().unwrap();
    let tp = |name, v| ast::Tparam {
        variance: Variance::Invariant,
        name,
        constraints: v,
        reified: ReifyKind::Erased,
        user_attributes: Default::default(),
    };

    // For polymorphic context with form `$g::C`
    // if $g's type is not a type parameter
    //   add one named "T/$g" constrained by $g's type
    //   replace $g's type hint
    // let Tg denote $g's final type (must be a type parameter).
    // add a type parameter "T/[$g::C]"
    // add a where constraint T/[$g::C] = Tg :: C
    let rewrite_arg_ctx = |env: &mut Env<'a>,
                           tparams: &mut Vec<ast::Tparam>,
                           where_constraints: &mut Vec<ast::WhereConstraintHint>,
                           hint: &mut Hint,
                           param_pos: &Pos,
                           name: &str,
                           context_pos: &Pos,
                           cst: &ast::Id| match *hint.1 {
        Happly(ast::Id(_, ref type_name), _) => {
            if !tparams.iter().any(|h| h.name.1 == *type_name) {
                // If the parameter is X $g, create tparam `T$g as X` and replace $g's type hint
                let id = ast::Id(param_pos.clone(), "T/".to_string() + name);
                tparams.push(tp(
                    id.clone(),
                    vec![(ast::ConstraintKind::ConstraintAs, hint.clone())],
                ));
                *hint = ast::Hint::new(param_pos.clone(), Happly(id, vec![]));
            };
            let right = ast::Hint::new(
                context_pos.clone(),
                Haccess(hint.clone(), vec![cst.clone()]),
            );
            let left_id = ast::Id(context_pos.clone(), format!("T/[{}::{}]", name, &cst.1));
            tparams.push(tp(left_id.clone(), vec![]));
            let left = ast::Hint::new(context_pos.clone(), Happly(left_id, vec![]));
            where_constraints.push(ast::WhereConstraintHint(
                left,
                ast::ConstraintKind::ConstraintEq,
                right,
            ))
        }
        _ => raise_parsing_error_pos(&hint.0, env, &syntax_error::ctx_var_invalid_type_hint(name)),
    };

    let mut hint_by_param: HashMap<&str, (&mut Option<ast::Hint>, &Pos, bool)> = HashMap::default();
    for param in params.iter_mut() {
        hint_by_param.insert(
            param.name.as_ref(),
            (
                &mut param.type_hint.1,
                &param.pos,
                match param.info {
                    ast::FunParamInfo::ParamVariadic => true,
                    _ => false,
                },
            ),
        );
    }

    for context_hint in context_hints {
        match *context_hint.1 {
            HfunContext(ref name) => match hint_by_param.get_mut::<str>(name) {
                Some((hint_opt, param_pos, _is_variadic)) => match hint_opt {
                    Some(_) if env.is_codegen() => {}
                    Some(param_hint) => rewrite_fun_ctx(env, tparams, param_hint, name),
                    None => raise_parsing_error_pos(
                        param_pos,
                        env,
                        &syntax_error::ctx_var_missing_type_hint(name),
                    ),
                },

                None => raise_parsing_error_pos(
                    &context_hint.0,
                    env,
                    &syntax_error::ctx_var_invalid_parameter(name),
                ),
            },
            Haccess(ref root, ref csts) => {
                if let Hvar(ref name) = *root.1 {
                    match hint_by_param.get_mut::<str>(name) {
                        Some((hint_opt, param_pos, is_variadic)) => {
                            if *is_variadic {
                                raise_parsing_error_pos(
                                    param_pos,
                                    env,
                                    &syntax_error::ctx_var_variadic(name),
                                )
                            } else {
                                match hint_opt {
                                    Some(_) if env.is_codegen() => {}
                                    Some(param_hint) => {
                                        let mut rewrite = |h| {
                                            rewrite_arg_ctx(
                                                env,
                                                tparams,
                                                where_constraints,
                                                h,
                                                param_pos,
                                                name,
                                                &context_hint.0,
                                                &csts[0],
                                            )
                                        };
                                        match *param_hint.1 {
                                            Hint_::Hlike(ref mut h) => match *h.1 {
                                                Hint_::Hoption(ref mut hinner) => rewrite(hinner),
                                                _ => rewrite(h),
                                            },
                                            Hint_::Hoption(ref mut h) => rewrite(h),
                                            _ => rewrite(param_hint),
                                        }
                                    }
                                    None => raise_parsing_error_pos(
                                        param_pos,
                                        env,
                                        &syntax_error::ctx_var_missing_type_hint(name),
                                    ),
                                }
                            }
                        }
                        None => raise_parsing_error_pos(
                            &root.0,
                            env,
                            &syntax_error::ctx_var_invalid_parameter(name),
                        ),
                    }
                } else if let Happly(ast::Id(_, ref id), _) = *root.1 {
                    // For polymorphic context with form `T::*::C` where `T` is a reified generic
                    // add a type parameter "T/[T::*::C]"
                    // add a where constraint T/[T::*::C] = T :: C
                    let haccess_string = |id, csts: &Vec<aast::Sid>| {
                        format!("{}::{}", id, csts.iter().map(|c| c.1.clone()).join("::"))
                    };

                    match env.get_reification(id) {
                        None => {} // not a generic
                        Some(false) => raise_parsing_error_pos(
                            &root.0,
                            env,
                            &syntax_error::ctx_generic_invalid(id, haccess_string(id, csts)),
                        ),
                        Some(true) if env.is_codegen() => {}
                        Some(true) => {
                            let left_id = ast::Id(
                                context_hint.0.clone(),
                                format!("T/[{}]", haccess_string(id, csts)),
                            );
                            tparams.push(tp(left_id.clone(), vec![]));
                            let left =
                                ast::Hint::new(context_hint.0.clone(), Happly(left_id, vec![]));
                            where_constraints.push(ast::WhereConstraintHint(
                                left,
                                ast::ConstraintKind::ConstraintEq,
                                context_hint.clone(),
                            ));
                        }
                    }
                }
            }
            _ => {}
        }
    }
}

fn p_fun_param_default_value<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<Option<ast::Expr>> {
    match &node.children {
        SimpleInitializer(c) => map_optional(&c.value, env, p_expr),
        _ => Ok(None),
    }
}

fn p_param_kind<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ParamKind> {
    match token_kind(node) {
        Some(TK::Inout) => Ok(ast::ParamKind::Pinout(p_pos(node, env))),
        None => Ok(ast::ParamKind::Pnormal),
        _ => missing_syntax("param kind", node, env),
    }
}

fn p_readonly<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ReadonlyKind> {
    match token_kind(node) {
        Some(TK::Readonly) => Ok(ast::ReadonlyKind::Readonly),
        _ => missing_syntax("readonly", node, env),
    }
}

fn p_named<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::ParamNamed> {
    match token_kind(node) {
        Some(TK::Named) => Ok(ast::ParamNamed::ParamNamed),
        _ => missing_syntax("named", node, env),
    }
}

fn p_splat<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::SplatKind> {
    match token_kind(node) {
        Some(TK::DotDotDot) => Ok(ast::SplatKind::Splat),
        _ => missing_syntax("...", node, env),
    }
}

fn p_optional<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::OptionalKind> {
    match token_kind(node) {
        Some(TK::Optional) => Ok(ast::OptionalKind::Optional),
        _ => missing_syntax("optional", node, env),
    }
}

fn param_template<'a>(node: S<'a>, env: &Env<'_>) -> ast::FunParam {
    let pos = p_pos(node, env);
    ast::FunParam {
        annotation: (),
        type_hint: ast::TypeHint((), None),
        pos,
        name: text(node, env),
        info: ast::FunParamInfo::ParamRequired,
        callconv: ast::ParamKind::Pnormal,
        readonly: None,
        splat: None,
        named: None,
        user_attributes: Default::default(),
        visibility: None,
    }
}

fn p_fun_param<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::FunParam> {
    match &node.children {
        ParameterDeclaration(ParameterDeclarationChildren {
            attribute,
            visibility,
            optional: _,
            call_convention: _,
            named: _,
            readonly: _,
            pre_ellipsis,
            type_,
            ellipsis,
            name,
            default_value,
            parameter_end: _parameter_end,
        }) => {
            let is_variadic = !ellipsis.is_missing();
            let user_attributes = p_user_attributes(attribute, env);
            let name = if name.is_missing() { ellipsis } else { name };
            let pos = p_pos(name, env);
            let name = text(name, env);
            let hint = map_optional(type_, env, p_hint)?;
            let hint = hint.map(|h| soften_hint(&user_attributes, h));

            if is_variadic && !user_attributes.is_empty() {
                raise_parsing_error(
                    node,
                    env,
                    &syntax_error::no_attributes_on_variadic_parameter,
                );
            }
            let expr = p_fun_param_default_value(default_value, env)?;
            let optional = parameter_modifier(node, TK::Optional);
            let callconv = parameter_modifier(node, TK::Inout)
                .map(|modifier| p_param_kind(modifier, env))
                .transpose()?
                .unwrap_or(ast::ParamKind::Pnormal);
            if optional.is_some() && expr.is_some() {
                raise_parsing_error(
                    node,
                    env,
                    &syntax_error::invalid_optional_keyword_on_initializer,
                );
            }
            if optional.is_some() && is_variadic {
                raise_parsing_error(node, env, &syntax_error::no_optional_on_variadic_parameter);
            }
            let info = match expr {
                _ if is_variadic => ast::FunParamInfo::ParamVariadic,
                Some(_) => ast::FunParamInfo::ParamOptional(expr),
                None if optional.is_some() => ast::FunParamInfo::ParamOptional(None),
                _ => ast::FunParamInfo::ParamRequired,
            };

            let splat = map_optional(pre_ellipsis, env, p_splat)?;
            Ok(ast::FunParam {
                annotation: (),
                type_hint: ast::TypeHint((), hint),
                user_attributes,
                pos,
                name,
                info,
                callconv,
                readonly: parameter_modifier(node, TK::Readonly)
                    .map(|modifier| p_readonly(modifier, env))
                    .transpose()?,
                splat,
                named: parameter_modifier(node, TK::Named)
                    .map(|modifier| p_named(modifier, env))
                    .transpose()?,
                /* implicit field via constructor parameter.
                 * This is always None except for constructors and the modifier
                 * can be only Public or Protected or Private.
                 */
                visibility: p_visibility(visibility, env),
            })
        }
        Token(_) if text_str(node, env) == "..." => {
            let mut param = param_template(node, env);
            param.info = ast::FunParamInfo::ParamVariadic;
            Ok(param)
        }
        _ => missing_syntax("function parameter", node, env),
    }
}

fn p_tconstraint_ty<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Hint> {
    match &node.children {
        TypeConstraint(c) => p_hint(&c.type_, env),
        _ => missing_syntax("type constraint", node, env),
    }
}

fn p_tconstraint<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<(ast::ConstraintKind, ast::Hint)> {
    match &node.children {
        TypeConstraint(c) => Ok((
            match token_kind(&c.keyword) {
                Some(TK::As) => ast::ConstraintKind::ConstraintAs,
                Some(TK::Super) => ast::ConstraintKind::ConstraintSuper,
                Some(TK::Equal) => ast::ConstraintKind::ConstraintEq,
                _ => missing_syntax("constraint operator", &c.keyword, env)?,
            },
            p_hint(&c.type_, env)?,
        )),
        _ => missing_syntax("type constraint", node, env),
    }
}

fn p_tparam<'a>(is_class: bool, node: S<'a>, env: &mut Env<'a>) -> Result<ast::Tparam> {
    match &node.children {
        TypeParameter(TypeParameterChildren {
            attribute_spec,
            reified,
            variance,
            name,
            constraints,
        }) => {
            let user_attributes = p_user_attributes(attribute_spec, env);
            let is_reified = !reified.is_missing();

            let type_name = text(name, env);
            if is_class {
                env.cls_generics_mut().insert(type_name, is_reified);
            } else {
                // this is incorrect for type aliases, but it doesn't affect any check
                env.fn_generics_mut().insert(type_name, is_reified);
            }

            let variance = match token_kind(variance) {
                Some(TK::Plus) => ast::Variance::Covariant,
                Some(TK::Minus) => ast::Variance::Contravariant,
                _ => ast::Variance::Invariant,
            };
            if is_reified && variance != ast::Variance::Invariant {
                raise_parsing_error(node, env, &syntax_error::non_invariant_reified_generic);
            }
            let reified = match (is_reified, has_soft(&user_attributes)) {
                (true, true) => ast::ReifyKind::SoftReified,
                (true, false) => ast::ReifyKind::Reified,
                _ => ast::ReifyKind::Erased,
            };
            Ok(ast::Tparam {
                variance,
                name: pos_name(name, env)?,
                constraints: could_map(constraints, env, p_tconstraint)?,
                reified,
                user_attributes,
            })
        }
        _ => missing_syntax("type parameter", node, env),
    }
}

fn p_tparam_l<'a>(is_class: bool, node: S<'a>, env: &mut Env<'a>) -> Result<Vec<ast::Tparam>> {
    match &node.children {
        Missing => Ok(vec![]),
        TypeParameters(c) => could_map(&c.parameters, env, |n, e| p_tparam(is_class, n, e)),
        _ => missing_syntax("type parameter", node, env),
    }
}

fn p_hint_tparam<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::HintTparam> {
    match &node.children {
        TypeParameter(TypeParameterChildren {
            attribute_spec,
            reified,
            variance,
            name,
            constraints,
        }) => {
            // User attributes with parameters, reified generics and
            // variance annotations are all disallowed
            if !reified.is_missing() {
                raise_parsing_error(node, env, &syntax_error::polymorphic_function_hint_reified);
            }
            if !variance.is_missing() {
                raise_parsing_error(node, env, &syntax_error::polymorphic_function_hint_variance);
            }

            let type_name = text(name, env);
            // this is incorrect for type aliases, but it doesn't affect any check
            env.fn_generics_mut().insert(type_name, false);

            let uas = p_user_attributes(attribute_spec, env);
            let user_attributes = uas
                .0
                .into_iter()
                .map(|attr| {
                    if !attr.params.is_empty() {
                        raise_parsing_error(
                            node,
                            env,
                            &syntax_error::polymorphic_function_hint_attribute_spec,
                        );
                    }
                    attr.name
                })
                .collect();

            Ok(ast::HintTparam {
                name: pos_name(name, env)?,
                constraints: could_map(constraints, env, p_tconstraint)?,
                user_attributes,
            })
        }
        _ => missing_syntax("type parameter", node, env),
    }
}

fn p_hint_tparam_l<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<Vec<ast::HintTparam>> {
    match &node.children {
        Missing => Ok(vec![]),
        TypeParameters(c) => could_map(&c.parameters, env, |n, e| p_hint_tparam(n, e)),
        _ => missing_syntax("type parameter", node, env),
    }
}

/// Lowers multiple constraints into a hint pair (lower_bound, upper_bound)
fn p_ctx_constraints<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<(Option<ast::Hint>, Option<ast::Hint>)> {
    let constraints = could_map(node, env, |node, env| {
        if let ContextConstraint(c) = &node.children {
            if let Some(hint) = p_context_list_to_intersection(
                &c.ctx_list,
                env,
                "Contexts cannot be bounded by polymorphic contexts",
            ) {
                Ok(match token_kind(&c.keyword) {
                    Some(TK::Super) => Either::Left(hint),
                    Some(TK::As) => Either::Right(hint),
                    _ => missing_syntax("constraint operator", &c.keyword, env)?,
                })
            } else {
                missing_syntax("contexts", &c.keyword, env)?
            }
        } else {
            missing_syntax("context constraint", node, env)?
        }
    })?;
    let (super_constraint, as_constraint) = constraints.into_iter().partition_map(|x| x);
    let require_one = &mut |kind: &str, cs: Vec<_>| {
        if cs.len() > 1 {
            let msg = format!(
                "Multiple `{}` constraints on a ctx constant are not allowed",
                kind
            );
            raise_parsing_error(node, env, &msg);
        }
        cs.into_iter().next()
    };
    Ok((
        require_one("super", super_constraint),
        require_one("as", as_constraint),
    ))
}

fn p_contexts<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
    error_on_polymorphic: Option<(&str, bool)>,
) -> Option<ast::Contexts> {
    match &node.children {
        Missing => None,
        Contexts(c) => {
            let hints = could_map_emit_error(&c.types, env, |node, env| {
                let h = p_hint(node, env)?;
                if let Some((e, ignore_this)) = error_on_polymorphic {
                    if is_polymorphic_context(env, &h, ignore_this) {
                        raise_parsing_error(node, env, e);
                    }
                }
                Ok(h)
            });
            let pos = p_pos(node, env);
            let ctxs = ast::Contexts(pos, hints);
            Some(ctxs)
        }
        _ => {
            raise_missing_syntax("contexts", node, env);
            None
        }
    }
}

fn p_context_list_to_intersection<'a>(
    ctx_list: S<'a>,
    env: &mut Env<'a>,
    polymorphic_error: &str,
) -> Option<ast::Hint> {
    p_contexts(ctx_list, env, Some((polymorphic_error, false)))
        .map(|t| ast::Hint::new(t.0, ast::Hint_::Hintersection(t.1)))
}

fn p_fun_hdr<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<FunHdr> {
    match &node.children {
        FunctionDeclarationHeader(FunctionDeclarationHeaderChildren {
            modifiers,
            name,
            where_clause,
            type_parameter_list,
            parameter_list,
            type_,
            contexts,
            readonly_return,
            ..
        }) => {
            if name.value.is_missing() {
                raise_parsing_error(name, env, &syntax_error::empty_method_name);
            }
            let kinds = p_kinds(modifiers, env);
            let has_async = kinds.has(modifier::ASYNC);
            let internal = kinds.has(modifier::INTERNAL);
            let readonly_this = if kinds.has(modifier::READONLY) {
                Some(ast::ReadonlyKind::Readonly)
            } else {
                None
            };
            let readonly_ret = map_optional(readonly_return, env, p_readonly)?;
            let mut type_parameters = p_tparam_l(false, type_parameter_list, env)?;
            let mut parameters = match could_map(parameter_list, env, p_fun_param) {
                Ok(params) => params,
                Err(e) => {
                    emit_error(e, env);
                    vec![]
                }
            };
            let contexts = p_contexts(contexts, env, None);
            let mut constrs = p_where_constraint(
                WhereConstraintSource::FunctionHeader,
                node,
                where_clause,
                env,
            )?;
            rewrite_effect_polymorphism(
                env,
                &mut parameters,
                &mut type_parameters,
                contexts.as_ref(),
                &mut constrs,
            );
            let return_type = map_optional(type_, env, p_hint)?;
            let suspension_kind = mk_suspension_kind_(has_async);
            let name = pos_name(name, env)?;
            let unsafe_contexts = contexts.clone();
            Ok(FunHdr {
                suspension_kind,
                readonly_this,
                name,
                internal,
                constrs,
                type_parameters,
                parameters,
                contexts,
                unsafe_contexts,
                readonly_return: readonly_ret,
                return_type,
            })
        }
        // TODO: this code seems to be dead, as the only callers of p_fun_hdr come from MethodishDeclaration and FunctionDeclaration
        LambdaSignature(LambdaSignatureChildren {
            parameters,
            contexts,
            type_,
            readonly_return,
            ..
        }) => {
            let readonly_ret = map_optional(readonly_return, env, p_readonly)?;
            let mut header = FunHdr::make_empty(env);
            header.parameters = could_map(parameters, env, p_fun_param)?;
            let contexts = p_contexts(contexts, env, None);
            let unsafe_contexts = contexts.clone();
            header.contexts = contexts;
            header.unsafe_contexts = unsafe_contexts;
            header.return_type = map_optional(type_, env, p_hint)?;
            header.readonly_return = readonly_ret;
            Ok(header)
        }
        Token(_) => Ok(FunHdr::make_empty(env)),
        _ => missing_syntax("function header", node, env),
    }
}

fn p_fun_pos<'a>(node: S<'a>, env: &Env<'_>) -> Pos {
    let get_pos = |n: S<'a>, p: Pos| -> Pos {
        if let FunctionDeclarationHeader(c1) = &n.children {
            if !c1.keyword.is_missing() {
                return Pos::btw_nocheck(p_pos(&c1.keyword, env), p);
            }
        }
        p
    };
    let p = p_pos(node, env);
    match &node.children {
        FunctionDeclaration(c) if env.is_codegen() => get_pos(&c.declaration_header, p),
        MethodishDeclaration(c) if env.is_codegen() => get_pos(&c.function_decl_header, p),
        _ => p,
    }
}

fn p_block<'a>(remove_noop: bool, node: S<'a>, env: &mut Env<'a>) -> Result<ast::Block> {
    let ast::Stmt(p, stmt_) = p_stmt(node, env)?;
    if let ast::Stmt_::Block(box (_, blk)) = stmt_ {
        if remove_noop && blk.len() == 1 && blk[0].1.is_noop() {
            return Ok(Default::default());
        }
        Ok(blk)
    } else {
        Ok(ast::Block(vec![ast::Stmt(p, stmt_)]))
    }
}

fn p_finally_block<'a>(
    remove_noop: bool,
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<ast::FinallyBlock> {
    let ast::Stmt(p, stmt_) = p_stmt(node, env)?;
    if let ast::Stmt_::Block(box (_, blk)) = stmt_ {
        check_continue_break(env, TK::Finally, &blk);
        if remove_noop && blk.len() == 1 && blk[0].1.is_noop() {
            return Ok(Default::default());
        }
        Ok(ast::FinallyBlock(blk.0))
    } else {
        Ok(ast::FinallyBlock(vec![ast::Stmt(p, stmt_)]))
    }
}

fn mk_noop(env: &Env<'_>) -> ast::Stmt {
    ast::Stmt::noop(env.mk_none_pos())
}

fn p_function_body<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::Block> {
    let mk_noop_result = |e: &Env<'_>| Ok(ast::Block(vec![mk_noop(e)]));
    match &node.children {
        Missing => Ok(Default::default()),
        CompoundStatement(c) => {
            let compound_statements = &c.statements.children;
            let compound_right_brace = &c.right_brace.children;
            match (compound_statements, compound_right_brace) {
                (Missing, Token(_)) => mk_noop_result(env),
                (SyntaxList(t), _) if t.len() == 1 && t[0].is_yield() => {
                    env.found_yield = true;
                    mk_noop_result(env)
                }
                _ => {
                    if !env.top_level_statements
                        && ((env.file_mode() == file_info::Mode::Mhhi && !env.is_codegen())
                            || env.quick_mode)
                    {
                        mk_noop_result(env)
                    } else {
                        p_block(false /*remove noop*/, node, env)
                    }
                }
            }
        }
        _ => {
            let expr = p_expr(node, env)?;
            Ok(ast::Block(vec![ast::Stmt::new(
                expr.1.clone(),
                ast::Stmt_::mk_return(Some(expr)),
            )]))
        }
    }
}

fn mk_suspension_kind<'a>(async_keyword: S<'a>) -> SuspensionKind {
    mk_suspension_kind_(!async_keyword.is_missing())
}

fn mk_suspension_kind_(has_async: bool) -> SuspensionKind {
    if has_async {
        SuspensionKind::SKAsync
    } else {
        SuspensionKind::SKSync
    }
}

fn mk_fun_kind(suspension_kind: SuspensionKind, yield_: bool) -> ast::FunKind {
    use SuspensionKind::*;
    use ast::FunKind::*;
    match (suspension_kind, yield_) {
        (SKSync, true) => FGenerator,
        (SKAsync, true) => FAsyncGenerator,
        (SKSync, false) => FSync,
        (SKAsync, false) => FAsync,
    }
}

fn process_attribute_constructor_call<'a>(
    node: S<'a>,
    constructor_call_argument_list: S<'a>,
    constructor_call_type: S<'a>,
    env: &mut Env<'a>,
) -> Result<ast::UserAttribute> {
    let name = pos_name(constructor_call_type, env)?;
    let params = could_map(constructor_call_argument_list, env, |n, e| {
        // Skip validation for __SimpliHack attributes as they may contain expressions not normally
        // accepted in attributes.
        if !sn::user_attributes::is_simplihack(&name.1) {
            is_valid_attribute_arg(n, e, &name.1)
        };
        p_expr(n, e)
    })?;
    if name.1.eq_ignore_ascii_case("__reified") || name.1.eq_ignore_ascii_case("__hasreifiedparent")
    {
        raise_parsing_error(node, env, &syntax_error::reified_attribute);
    } else if name.1.eq_ignore_ascii_case(special_attrs::SOFT)
        && constructor_call_argument_list
            .syntax_node_to_list_skip_separator()
            .count()
            > 0
    {
        raise_parsing_error(node, env, &syntax_error::soft_no_arguments);
    } else if sn::user_attributes::is_memoized(&name.1) {
        let list: Vec<_> = constructor_call_argument_list
            .syntax_node_to_list_skip_separator()
            .collect();

        if let Some(first_arg) = list.first() {
            if !matches!(first_arg.children, EnumClassLabelExpression(_)) {
                raise_parsing_error(
                    first_arg,
                    env,
                    &syntax_error::memoize_requires_label(&name.1),
                );
            }
        }

        if list.len() > 1 {
            let ast::Id(_, first) = pos_name(list[0], env)?;
            raise_parsing_error(
                list[1],
                env,
                &syntax_error::memoize_invalid_arity(&name.1, 1, &first),
            );
        }
    }
    Ok(ast::UserAttribute { name, params })
}

// Arguments to attributes must be literals (int, string, etc), collections
// (eg vec, dict, keyset, etc), Foo::class strings, shapes, string
// concatenations, or tuples.
fn is_valid_attribute_arg<'a>(node: S<'a>, env: &mut Env<'a>, attr_name: &str) {
    let is_valid_list = |nodes: S<'a>, env: &mut Env<'a>| {
        let _ = could_map(nodes, env, |n, e| {
            is_valid_attribute_arg(n, e, attr_name);
            Ok(())
        });
    };
    match &node.children {
        ParenthesizedExpression(c) => is_valid_attribute_arg(&c.expression, env, attr_name),
        // Normal literals (string, int, etc)
        LiteralExpression(_) => {}
        // Only allow enum class label syntax on __Memoize and __MemizeLSB.
        EnumClassLabelExpression(ecl) if sn::user_attributes::is_memoized(attr_name) => {
            if let Ok(ast::Id(_, label_name)) = pos_name(&ecl.expression, env) {
                if !sn::memoize_option::is_valid(&label_name) {
                    raise_parsing_error(node, env, &syntax_error::memoize_invalid_label(attr_name));
                }
            }
        }
        NameofExpression(_) => {} // matches the ::class case
        // ::class strings
        ScopeResolutionExpression(c) => {
            if let Some(TK::Class) = token_kind(&c.name) {
            } else {
                raise_parsing_error(node, env, &syntax_error::expression_as_attribute_arguments);
            }
        }
        // Negations
        PrefixUnaryExpression(c) => {
            is_valid_attribute_arg(&c.operand, env, attr_name);
            match token_kind(&c.operator) {
                Some(TK::Minus) => {}
                Some(TK::Plus) => {}
                _ => {
                    raise_parsing_error(node, env, &syntax_error::expression_as_attribute_arguments)
                }
            }
        }
        // String concatenation
        BinaryExpression(c) => {
            if let Some(TK::Dot) = token_kind(&c.operator) {
                is_valid_attribute_arg(&c.left_operand, env, attr_name);
                is_valid_attribute_arg(&c.right_operand, env, attr_name);
            } else {
                raise_parsing_error(node, env, &syntax_error::expression_as_attribute_arguments);
            }
        }
        // Top-level Collections
        DarrayIntrinsicExpression(c) => is_valid_list(&c.members, env),
        DictionaryIntrinsicExpression(c) => is_valid_list(&c.members, env),
        KeysetIntrinsicExpression(c) => is_valid_list(&c.members, env),
        VarrayIntrinsicExpression(c) => is_valid_list(&c.members, env),
        VectorIntrinsicExpression(c) => is_valid_list(&c.members, env),
        ShapeExpression(c) => is_valid_list(&c.fields, env),
        TupleExpression(c) => is_valid_list(&c.items, env),
        // Collection Internals
        FieldInitializer(c) => {
            is_valid_attribute_arg(&c.name, env, attr_name);
            is_valid_attribute_arg(&c.value, env, attr_name);
        }
        ElementInitializer(c) => {
            is_valid_attribute_arg(&c.key, env, attr_name);
            is_valid_attribute_arg(&c.value, env, attr_name);
        }
        // Everything else is not allowed
        _ => raise_parsing_error(node, env, &syntax_error::expression_as_attribute_arguments),
    }
}

fn p_user_attribute<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::UserAttributes> {
    let p_attr = |n: S<'a>, e: &mut Env<'a>| -> Result<ast::UserAttribute> {
        match &n.children {
            ConstructorCall(c) => {
                process_attribute_constructor_call(node, &c.argument_list, &c.type_, e)
            }
            _ => missing_syntax("attribute", node, e),
        }
    };
    match &node.children {
        FileAttributeSpecification(c) => could_map(&c.attributes, env, p_attr),
        OldAttributeSpecification(c) => could_map(&c.attributes, env, p_attr),
        _ => missing_syntax("attribute specification", node, env),
    }
    .map(ast::UserAttributes)
}

fn p_user_attributes<'a>(node: S<'a>, env: &mut Env<'a>) -> ast::UserAttributes {
    let attributes = could_map_emit_error(node, env, p_user_attribute);
    let attributes = attributes.into_iter().flatten();

    // Drop SimpliHack attributes when generating code
    if env.is_codegen() {
        attributes
            .filter(|attr| !sn::user_attributes::is_simplihack(&attr.name.1))
            .collect()
    } else {
        attributes.collect()
    }
}

/// Extract the URL in `<<__Docs("http://example.com")>>` if the __Docs attribute
/// is present.
fn p_docs_url<'a>(attrs: &ast::UserAttributes, env: &mut Env<'a>) -> Option<String> {
    let mut url = None;

    for attr in attrs {
        if attr.name.1 == sn::user_attributes::DOCS {
            match attr.params.as_slice() {
                [param] => match &param.2 {
                    ast::Expr_::String(s) => match String::from_utf8(s.to_vec()) {
                        Ok(s) => {
                            url = Some(s);
                        }
                        Err(_) => raise_parsing_error_pos(
                            &attr.name.0,
                            env,
                            "`__Docs` URLs must be valid UTF-8",
                        ),
                    },
                    _ => raise_parsing_error_pos(
                        &attr.name.0,
                        env,
                        "`__Docs` URLs must be a string literal",
                    ),
                },
                _ => {
                    // Wrong number of arguments to __Docs,
                    // ignore. The attribute arity checks will tell
                    // the user their code is wrong.
                }
            }
        }
    }

    url
}

fn mk_empty_ns_env(env: &Env<'_>) -> Arc<NamespaceEnv> {
    Arc::clone(&env.empty_ns_env)
}

fn extract_docblock<'a>(node: S<'a>, env: &Env<'_>) -> Option<DocComment> {
    #[derive(Copy, Clone, Eq, PartialEq)]
    enum ScanState {
        DocComment,
        EmbeddedCmt,
        EndDoc,
        EndEmbedded,
        Free,
        LineCmt,
        MaybeDoc,
        MaybeDoc2,
        SawSlash,
    }
    use ScanState::*;
    // `parse` mixes loop and recursion to use less stack space.
    fn parse(
        str: &str,
        start: usize,
        state: ScanState,
        idx: usize,
    ) -> Option<(usize, usize, String)> {
        let is_whitespace = |c| c == ' ' || c == '\t' || c == '\n' || c == '\r';
        let mut s = (start, state, idx);
        let chars = str.as_bytes();
        loop {
            if s.2 == str.len() {
                break None;
            }

            let next = s.2 + 1;
            match (s.1, chars[s.2] as char) {
                (LineCmt, '\n') => s = (next, Free, next),
                (EndEmbedded, '/') => s = (next, Free, next),
                (EndDoc, '/') => {
                    let r = parse(str, next, Free, next);
                    match r {
                        d @ Some(_) => break d,
                        None => break Some((s.0, s.2 + 1, String::from(&str[s.0..s.2 + 1]))),
                    }
                }
                /* PHP has line comments delimited by a # */
                (Free, '#') => s = (next, LineCmt, next),
                /* All other comment delimiters start with a / */
                (Free, '/') => s = (s.2, SawSlash, next),
                /* After a / in trivia, we must see either another / or a * */
                (SawSlash, '/') => s = (next, LineCmt, next),
                (SawSlash, '*') => s = (s.0, MaybeDoc, next),
                (MaybeDoc, '*') => s = (s.0, MaybeDoc2, next),
                (MaybeDoc, _) => s = (s.0, EmbeddedCmt, next),
                (MaybeDoc2, '/') => s = (next, Free, next),
                /* Doc comments have a space after the second star */
                (MaybeDoc2, c) if is_whitespace(c) => s = (s.0, DocComment, s.2),
                (MaybeDoc2, _) => s = (s.0, EmbeddedCmt, next),
                (DocComment, '*') => s = (s.0, EndDoc, next),
                (DocComment, _) => s = (s.0, DocComment, next),
                (EndDoc, _) => s = (s.0, DocComment, next),
                /* A * without a / does not end an embedded comment */
                (EmbeddedCmt, '*') => s = (s.0, EndEmbedded, next),
                (EndEmbedded, '*') => s = (s.0, EndEmbedded, next),
                (EndEmbedded, _) => s = (s.0, EmbeddedCmt, next),
                /* Whitespace skips everywhere else */
                (_, c) if is_whitespace(c) => s = (s.0, s.1, next),
                /* When scanning comments, anything else is accepted */
                (LineCmt, _) => s = (s.0, s.1, next),
                (EmbeddedCmt, _) => s = (s.0, s.1, next),
                _ => break None,
            }
        }
    }
    let str = node.leading_text(env.indexed_source_text.source_text());
    parse(str, 0, Free, 0).map(|(start, end, txt)| {
        let anchor = node.leading_start_offset();
        let pos = env
            .indexed_source_text
            .relative_pos(anchor + start, anchor + end)
            .into();
        (pos, txt)
    })
}

fn p_xhp_child<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<ast::XhpChild> {
    use ast::XhpChild::*;
    use ast::XhpChildOp::*;
    match &node.children {
        Token(_) => pos_name(node, env).map(ChildName),
        PostfixUnaryExpression(c) => {
            let operand = p_xhp_child(&c.operand, env)?;
            let operator = match token_kind(&c.operator) {
                Some(TK::Question) => ChildQuestion,
                Some(TK::Plus) => ChildPlus,
                Some(TK::Star) => ChildStar,
                _ => missing_syntax("xhp children operator", node, env)?,
            };
            Ok(ChildUnary(Box::new(operand), operator))
        }
        BinaryExpression(c) => {
            let left = p_xhp_child(&c.left_operand, env)?;
            let right = p_xhp_child(&c.right_operand, env)?;
            Ok(ChildBinary(Box::new(left), Box::new(right)))
        }
        XHPChildrenParenthesizedList(c) => {
            let children: Result<Vec<_>, _> = c
                .xhp_children
                .syntax_node_to_list_skip_separator()
                .map(|c| p_xhp_child(c, env))
                .collect();
            Ok(ChildList(children?))
        }
        _ => missing_syntax("xhp children", node, env),
    }
}

fn p_tconstraints_into_lower_and_upper<'a>(
    node: S<'a>,
    env: &mut Env<'a>,
) -> (Vec<ast::Hint>, Vec<ast::Hint>) {
    let mut lower = vec![];
    let mut upper = vec![];
    for constraint in node.syntax_node_to_list_skip_separator() {
        let (kind, ty) = match p_tconstraint(constraint, env) {
            Ok(v) => v,
            Err(e) => {
                emit_error(e, env);
                continue;
            }
        };
        match kind {
            ast::ConstraintKind::ConstraintAs => upper.push(ty),
            ast::ConstraintKind::ConstraintSuper => lower.push(ty),
            _ => (),
        };
    }
    (lower, upper)
}

fn merge_constraints(
    mut constraints: Vec<ast::Hint>,
    f: fn(Vec<ast::Hint>) -> ast::Hint_,
) -> Option<ast::Hint> {
    if constraints.len() == 1 {
        constraints.pop()
    } else {
        #[allow(clippy::manual_map)]
        // map doesn't allow moving out of borrowed constraints
        match constraints.first() {
            None => None, // no bounds
            Some(fst) => Some(ast::Hint::new(fst.0.clone(), f(constraints))),
        }
    }
}

fn p_method_vis<'a>(node: S<'a>, name_pos: &Pos, env: &mut Env<'a>) -> ast::Visibility {
    match p_visibility_last_win(node, env) {
        None => {
            let first_token_pos = match node.syntax_node_to_list_skip_separator().next() {
                Some(token_node) => p_pos(token_node, env),
                None => name_pos.clone(),
            };

            raise_hh_error(
                env,
                Naming::method_needs_visibility(first_token_pos, name_pos.clone()),
            );
            ast::Visibility::Public
        }
        Some(v) => v,
    }
}

fn has_fun_header(
    m: &MethodishDeclarationChildren<'_, PositionedToken<'_>, PositionedValue<'_>>,
) -> bool {
    matches!(
        m.function_decl_header.children,
        FunctionDeclarationHeader(_)
    )
}

fn p_xhp_class_attr<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<Either<ast::XhpAttr, ast::Hint>> {
    let mk_attr_use = |n: S<'a>, env: &mut Env<'a>| {
        Ok(Either::Right(ast::Hint(
            p_pos(n, env),
            Box::new(ast::Hint_::Happly(pos_name(n, env)?, vec![])),
        )))
    };
    match &node.children {
        XHPClassAttribute(c) => {
            let ast::Id(p, name) = pos_name(&c.name, env)?;
            if let TypeConstant(_) = &c.type_.children {
                use oxidized::experimental_features::FeatureName;
                if !env
                    .active_experimental_features
                    .contains(&FeatureName::XhpTypeConstants)
                    && env.is_typechecker()
                {
                    raise_parsing_error(
                        &c.type_,
                        env,
                        &syntax_error::xhp_class_attribute_type_constant,
                    )
                }
            }
            let req = match &c.required.children {
                XHPRequired(_) => Some(ast::XhpAttrTag::Required),
                XHPLateinit(_) => Some(ast::XhpAttrTag::LateInit),
                _ => None,
            };
            let pos = if c.initializer.is_missing() {
                p.clone()
            } else {
                Pos::btw(&p, &p_pos(&c.initializer, env)).map_err(|message| {
                    Error::ParsingError {
                        message,
                        pos: p.clone(),
                    }
                })?
            };
            let (hint, like, enum_values, enum_) = match &c.type_.children {
                XHPEnumType(c1) => {
                    let p = p_pos(&c.type_, env);
                    let like = match &c1.like.children {
                        Missing => None,
                        _ => Some(p_pos(&c1.like, env)),
                    };
                    let vals = could_map(&c1.values, env, p_expr)?;
                    let mut enum_vals = vec![];
                    for val in vals.clone() {
                        match val {
                            ast::Expr(_, _, Expr_::String(xev)) => {
                                enum_vals.push(ast::XhpEnumValue::XEVString(xev.to_string()))
                            }
                            ast::Expr(_, _, Expr_::Int(xev)) => match xev.parse() {
                                Ok(n) => enum_vals.push(ast::XhpEnumValue::XEVInt(n)),
                                Err(_) =>
                                    // Since we have parse checks for
                                // malformed integer literals already,
                                // we assume this won't happen and ignore
                                // the case.
                                    {}
                            },
                            _ => {}
                        }
                    }
                    (None, like, enum_vals, Some((p, vals)))
                }
                _ => (Some(p_hint(&c.type_, env)?), None, vec![], None),
            };
            let init_expr = map_optional(&c.initializer, env, p_simple_initializer)?;
            let xhp_attr = ast::XhpAttr(
                ast::TypeHint((), hint.clone()),
                ast::ClassVar {
                    final_: false,
                    xhp_attr: Some(ast::XhpAttrInfo {
                        like,
                        tag: req,
                        enum_values,
                    }),
                    abstract_: false,
                    readonly: false,
                    visibility: ast::Visibility::Public,
                    type_: ast::TypeHint((), hint),
                    id: ast::Id(p, String::from(":") + &name),
                    expr: init_expr,
                    user_attributes: Default::default(),
                    doc_comment: None,
                    is_static: false,
                    span: pos,
                },
                req,
                enum_,
            );
            Ok(Either::Left(xhp_attr))
        }
        XHPSimpleClassAttribute(c) => mk_attr_use(&c.type_, env),
        Token(_) => mk_attr_use(node, env),
        _ => missing_syntax("XHP attribute", node, env),
    }
}

fn p_type_constant<'a>(
    node: S<'a>,
    doc_comment_opt: Option<DocComment>,
    env: &mut Env<'a>,
    cls: &mut ast::Class_,
) {
    match &node.children {
        TypeConstDeclaration(c) => {
            use ast::ClassTypeconst::TCAbstract;
            use ast::ClassTypeconst::TCConcrete;
            if !c.type_parameters.is_missing() {
                raise_parsing_error(node, env, &syntax_error::tparams_in_tconst);
            }
            let user_attributes = p_user_attributes(&c.attribute_spec, env);
            let type__ = map_optional_emit_error(&c.type_specifier, env, p_hint)
                .map(|hint| soften_hint(&user_attributes, hint));
            let kinds = p_kinds(&c.modifiers, env);
            let name = match pos_name(&c.name, env) {
                Ok(name) => name,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };

            // desugar multiple same-kinded constraints as folows:
            let (lower, upper) = p_tconstraints_into_lower_and_upper(&c.type_constraints, env);
            // `as num as T1` -> `as (num & T1)`
            let as_constraint = merge_constraints(upper, ast::Hint_::Hintersection);
            // `super int as T2` -> `as (int | T2)`
            let super_constraint = merge_constraints(lower, ast::Hint_::Hunion);

            let span = p_pos(node, env);
            let has_abstract = kinds.has(modifier::ABSTRACT);
            let kind = if has_abstract {
                TCAbstract(ast::ClassAbstractTypeconst {
                    as_constraint,
                    super_constraint,
                    default: type__,
                })
            } else if let Some(type_) = type__ {
                if env.is_typechecker() && (as_constraint.is_some() || super_constraint.is_some()) {
                    raise_hh_error(
                        env,
                        NastCheck::partially_abstract_typeconst_definition(
                            name.0.clone(),
                            if as_constraint.is_some() {
                                "as"
                            } else {
                                "super"
                            },
                        ),
                    );
                }
                TCConcrete(ast::ClassConcreteTypeconst { c_tc_type: type_ })
            } else {
                raise_hh_error(env, NastCheck::not_abstract_without_typeconst(name.0));
                raise_missing_syntax("value for the type constant", node, env);
                return;
            };
            cls.typeconsts.push(ast::ClassTypeconstDef {
                name,
                kind,
                user_attributes,
                span,
                doc_comment: doc_comment_opt,
                is_ctx: false,
            })
        }
        _ => {}
    }
}

fn is_valid_trait_req_class_name<'a>(
    hint: &ast::Hint,
    c_name: S<'a>,
    env: &mut Env<'a>,
    is_this_as: bool,
) -> bool {
    use ast::Hint_;

    let ast::Hint(_pos, hint_) = hint;
    let error = if is_this_as {
        &syntax_error::require_this_as_applied_to_generic
    } else {
        &syntax_error::require_class_applied_to_generic
    };
    match hint_.as_ref() {
        Hint_::Happly(_, v) => {
            if !(v.is_empty()) {
                /* in a `require class t;` or `require this <: t;` trait constraint,
                t must be a non-generic class name */
                raise_parsing_error(c_name, env, error)
            };
            true
        }
        _ => {
            raise_missing_syntax("class name", c_name, env);
            false
        }
    }
}

/// Given an FFP `node` that represents a class element (e.g a
/// property, a method or a class constant), lower it to the
/// equivalent AAST representation and store in `class`.
///
/// If we encounter an error, write the error to `env` and don't add
/// anything to `class`.
fn p_class_elt<'a>(class: &mut ast::Class_, node: S<'a>, env: &mut Env<'a>) {
    let doc_comment_opt = extract_docblock(node, env);

    match &node.children {
        ConstDeclaration(c) => {
            let user_attributes = p_user_attributes(&c.attribute_spec, env);
            let kinds = p_kinds(&c.modifiers, env);
            let has_abstract = kinds.has(modifier::ABSTRACT);
            // TODO: make wrap `type_` `doc_comment` by `Rc` in ClassConst to avoid clone
            let type_ = map_optional_emit_error(&c.type_specifier, env, p_hint);
            let span = p_pos(node, env);

            let mut class_consts =
                could_map_emit_error(&c.declarators, env, |n, e| match &n.children {
                    ConstantDeclarator(c) => {
                        let id = pos_name(&c.name, e)?;
                        let pos = &id.0;
                        use aast::ClassConstKind::*;
                        let kind = if has_abstract {
                            CCAbstract(map_optional(&c.initializer, e, p_simple_initializer)?)
                        } else {
                            CCConcrete(p_const_value(&c.initializer, e, pos.clone())?)
                        };
                        Ok(ast::ClassConst {
                            user_attributes: user_attributes.clone(),
                            type_: type_.clone(),
                            id,
                            kind,
                            span: span.clone(),
                            doc_comment: doc_comment_opt.clone(),
                        })
                    }
                    _ => missing_syntax("constant declarator", n, e),
                });
            class.consts.append(&mut class_consts)
        }
        TypeConstDeclaration(_) => p_type_constant(node, doc_comment_opt, env, class),
        ContextConstDeclaration(c) => {
            use ast::ClassTypeconst::TCAbstract;
            use ast::ClassTypeconst::TCConcrete;
            if !c.type_parameters.is_missing() {
                raise_parsing_error(node, env, &syntax_error::tparams_in_tconst);
            }
            let name = match pos_name(&c.name, env) {
                Ok(name) => name,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };
            let context = p_context_list_to_intersection(
                &c.ctx_list,
                env,
                "Context constants cannot alias polymorphic contexts",
            );
            if let Some(ref hint) = context {
                use ast::Hint_::Happly;
                use ast::Hint_::Hintersection;
                let ast::Hint(_, h) = hint;
                if let Hintersection(hl) = &**h {
                    for h in hl {
                        let ast::Hint(_, h) = h;
                        if let Happly(oxidized::ast::Id(_, id), _) = &**h {
                            if id.as_str().ends_with("_local") {
                                raise_parsing_error(
                                    &c.ctx_list,
                                    env,
                                    "Local contexts on ctx constants are not allowed",
                                );
                            }
                        }
                    }
                }
            }
            let span = p_pos(node, env);
            let kinds = p_kinds(&c.modifiers, env);
            let has_abstract = kinds.has(modifier::ABSTRACT);
            let (super_constraint, as_constraint) =
                p_ctx_constraints(&c.constraint, env).unwrap_or((None, None));
            let kind = if has_abstract {
                TCAbstract(ast::ClassAbstractTypeconst {
                    as_constraint,
                    super_constraint,
                    default: context,
                })
            } else if let Some(c_tc_type) = context {
                if env.is_typechecker() && (super_constraint.is_some() || as_constraint.is_some()) {
                    raise_parsing_error(
                        node,
                        env,
                        "Constraints on a context constant requires it to be abstract",
                    )
                };
                TCConcrete(ast::ClassConcreteTypeconst { c_tc_type })
            } else {
                raise_hh_error(env, NastCheck::not_abstract_without_typeconst(name.0));
                raise_missing_syntax("value for the context constant", node, env);
                return;
            };
            class.typeconsts.push(ast::ClassTypeconstDef {
                name,
                kind,
                user_attributes: Default::default(),
                span,
                doc_comment: doc_comment_opt,
                is_ctx: true,
            });
        }
        PropertyDeclaration(c) => {
            let user_attributes = p_user_attributes(&c.attribute_spec, env);
            let type_ = map_optional_emit_error(&c.type_, env, p_hint)
                .map(|t| soften_hint(&user_attributes, t));
            let kinds = p_kinds(&c.modifiers, env);
            let vis = p_visibility_last_win_or(&c.modifiers, env, ast::Visibility::Public);
            let doc_comment = if env.quick_mode {
                None
            } else {
                doc_comment_opt
            };
            let name_exprs = could_map_emit_error(&c.declarators, env, |n, e| match &n.children {
                PropertyDeclarator(c) => {
                    let name = pos_name_(&c.name, e, Some('$'))?;
                    let pos = p_pos(n, e);
                    let expr = map_optional(&c.initializer, e, p_simple_initializer)?;
                    Ok((pos, name, expr))
                }
                _ => missing_syntax("property declarator", n, e),
            });

            for (i, name_expr) in name_exprs.into_iter().enumerate() {
                class.vars.push(ast::ClassVar {
                    final_: kinds.has(modifier::FINAL),
                    xhp_attr: None,
                    abstract_: kinds.has(modifier::ABSTRACT),
                    readonly: kinds.has(modifier::READONLY),
                    visibility: vis,
                    type_: ast::TypeHint((), type_.clone()),
                    id: name_expr.1,
                    expr: name_expr.2,
                    user_attributes: user_attributes.clone(),
                    doc_comment: if i == 0 { doc_comment.clone() } else { None },
                    is_static: kinds.has(modifier::STATIC),
                    span: name_expr.0,
                });
            }
        }
        MethodishDeclaration(c) if has_fun_header(c) => {
            // keep cls_generics
            *env.fn_generics_mut() = HashMap::default();
            let classvar_init = |param: &ast::FunParam| -> (ast::Stmt, ast::ClassVar) {
                let cvname = drop_prefix(&param.name, '$');
                let p = &param.pos;
                let span = match &param.info {
                    ast::FunParamInfo::ParamOptional(Some(ast::Expr(_, pos_end, _))) => {
                        Pos::btw(p, pos_end).unwrap_or_else(|_| p.clone())
                    }
                    _ => p.clone(),
                };
                let e = |expr_: Expr_| -> ast::Expr { ast::Expr::new((), p.clone(), expr_) };
                let prop_type_hint = match (&param.type_hint.1, &param.info) {
                    (Some(h), ast::FunParamInfo::ParamVariadic) => ast::TypeHint(
                        (),
                        Some(ast::Hint::new(
                            param.pos.clone(),
                            ast::Hint_::mk_happly(
                                ast::Id(param.pos.clone(), String::from("vec")),
                                vec![h.clone()],
                            ),
                        )),
                    ),
                    _ => param.type_hint.clone(),
                };
                let lid = |s: &str| -> ast::Lid { ast::Lid(p.clone(), (0, s.to_string())) };
                (
                    ast::Stmt::new(
                        p.clone(),
                        ast::Stmt_::mk_expr(e(Expr_::mk_assign(
                            e(Expr_::mk_obj_get(
                                e(Expr_::mk_lvar(lid(special_idents::THIS))),
                                e(Expr_::mk_id(ast::Id(p.clone(), cvname.to_string()))),
                                ast::OperatorNullFlavor::Regular,
                                ast::PropOrMethod::IsProp,
                            )),
                            None,
                            e(Expr_::mk_lvar(lid(&param.name))),
                        ))),
                    ),
                    ast::ClassVar {
                        final_: false,
                        xhp_attr: None,
                        abstract_: false,
                        // We use the param readonlyness here to represent the
                        // ClassVar's readonlyness once lowered
                        // TODO(jjwu): Convert this to an enum when we support
                        // multiple types of readonlyness
                        readonly: param.readonly.is_some(),
                        visibility: param.visibility.unwrap(),
                        type_: prop_type_hint,
                        id: ast::Id(p.clone(), cvname.to_string()),
                        expr: None,
                        user_attributes: param.user_attributes.clone(),
                        doc_comment: None,
                        is_static: false,
                        span,
                    },
                )
            };
            let header = &c.function_decl_header;
            let h = match &header.children {
                FunctionDeclarationHeader(h) => h,
                _ => panic!(),
            };
            let hdr = match p_fun_hdr(header, env) {
                Ok(hdr) => hdr,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };
            let (mut member_init, mut member_def): (Vec<ast::Stmt>, Vec<ast::ClassVar>) = hdr
                .parameters
                .iter()
                .filter_map(|p| p.visibility.map(|_| classvar_init(p)))
                .unzip();

            let kinds = p_kinds(&h.modifiers, env);
            let visibility = p_method_vis(&h.modifiers, &hdr.name.0, env);
            let is_static = kinds.has(modifier::STATIC);
            let readonly_this = kinds.has(modifier::READONLY);
            *env.in_static_method() = is_static;
            check_effect_polymorphic_reification(hdr.contexts.as_ref(), env, node);
            let (mut body, body_has_yield) = match env.reset_for_function_body(
                &c.function_body,
                hdr.suspension_kind,
                p_function_body,
            ) {
                Ok(value) => value,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };
            if env.is_codegen() {
                member_init.reverse();
            }
            member_init.append(&mut body.0);
            let body = ast::Block(member_init);
            *env.in_static_method() = false;
            let is_abstract = kinds.has(modifier::ABSTRACT);
            let is_external = !is_abstract && c.function_body.is_external();
            let fun_kind = mk_fun_kind(hdr.suspension_kind, body_has_yield);
            let mut user_attributes = p_user_attributes(&c.attribute, env);
            populate_memoize_default(hdr.contexts.as_ref(), &mut user_attributes, env);
            check_effect_memoized(hdr.contexts.as_ref(), &user_attributes, "method", env);
            check_asio_lowpri(fun_kind, &user_attributes, env);
            let method = ast::Method_ {
                span: p_fun_pos(node, env),
                annotation: (),
                final_: kinds.has(modifier::FINAL),
                readonly_this,
                abstract_: is_abstract,
                static_: is_static,
                name: hdr.name,
                visibility,
                tparams: hdr.type_parameters,
                where_constraints: hdr.constrs,
                params: hdr.parameters,
                ctxs: hdr.contexts,
                unsafe_ctxs: hdr.unsafe_contexts,
                body: ast::FuncBody { fb_ast: body },
                fun_kind,
                user_attributes,
                readonly_ret: hdr.readonly_return,
                ret: ast::TypeHint((), hdr.return_type),
                external: is_external,
                hidden: false,
                doc_comment: doc_comment_opt,
            };
            class.vars.append(&mut member_def);
            class.methods.push(method)
        }
        TraitUse(c) => {
            let mut uses = could_map_emit_error(&c.names, env, p_hint);
            class.uses.append(&mut uses)
        }
        RequireClause(c) => {
            use aast::RequireKind::*;
            let hint = match p_hint(&c.name, env) {
                Ok(hint) => hint,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };
            let require_kind = match token_kind(&c.kind) {
                Some(TK::Implements) => Some(RequireImplements),
                Some(TK::Extends) => Some(RequireExtends),
                Some(TK::Class) => {
                    if is_valid_trait_req_class_name(&hint, &c.name, env, false) {
                        Some(RequireClass)
                    } else {
                        None
                    }
                }
                _ => {
                    raise_missing_syntax("trait require kind", &c.kind, env);
                    None
                }
            };
            if let Some(require_kind) = require_kind {
                class.reqs.push(ClassReq(hint, require_kind));
            }
        }
        RequireClauseConstraint(c) => {
            use aast::RequireKind::*;
            let hint = match p_hint(&c.name, env) {
                Ok(hint) => hint,
                Err(e) => {
                    emit_error(e, env);
                    return;
                }
            };
            is_valid_trait_req_class_name(&hint, &c.name, env, true);
            class.reqs.push(ClassReq(hint, RequireThisAs));
        }
        XHPClassAttributeDeclaration(c) => {
            let attrs = could_map_emit_error(&c.attributes, env, p_xhp_class_attr);
            for attr in attrs.into_iter() {
                match attr {
                    Either::Left(attr) => class.xhp_attrs.push(attr),
                    Either::Right(xhp_attr_use) => class.xhp_attr_uses.push(xhp_attr_use),
                }
            }
        }
        XHPChildrenDeclaration(c) => {
            let p = p_pos(node, env);
            match p_xhp_child(&c.expression, env) {
                Ok(child) => {
                    class.xhp_children.push((p, child));
                }
                Err(e) => {
                    emit_error(e, env);
                }
            }
        }
        XHPCategoryDeclaration(c) => {
            let p = p_pos(node, env);

            let categories =
                could_map_emit_error(&c.categories, env, |n, e| p_pstring_(n, e, Some('%')));
            if let Some((_, cs)) = &class.xhp_category {
                if let Some(category) = cs.first() {
                    raise_hh_error(env, NastCheck::multiple_xhp_category(category.0.clone()))
                }
            }
            class.xhp_category = Some((p, categories))
        }
        _ => raise_missing_syntax("class element", node, env),
    }
}

fn contains_class_body<'a>(
    c: &ClassishDeclarationChildren<'_, PositionedToken<'a>, PositionedValue<'a>>,
) -> bool {
    matches!(&c.body.children, ClassishBody(_))
}

enum WhereConstraintSource {
    CaseTypeVariant,
    FunctionHeader,
}

impl WhereConstraintSource {
    fn as_str(&self) -> &'static str {
        match self {
            WhereConstraintSource::CaseTypeVariant => "case type variant",
            WhereConstraintSource::FunctionHeader => "function header",
        }
    }
}

fn p_where_constraint<'a>(
    parent_kind: WhereConstraintSource,
    parent: S<'a>,
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<Vec<ast::WhereConstraintHint>> {
    match &node.children {
        Missing => Ok(vec![]),
        WhereClause(c) => {
            let f = |n: S<'a>, e: &mut Env<'a>| -> Result<ast::WhereConstraintHint> {
                match &n.children {
                    WhereConstraint(c) => {
                        use ast::ConstraintKind::*;
                        let l = p_hint(&c.left_type, e)?;
                        let o = &c.operator;
                        let o = match token_kind(o) {
                            Some(TK::Equal) => ConstraintEq,
                            Some(TK::As) => ConstraintAs,
                            Some(TK::Super) => ConstraintSuper,
                            _ => missing_syntax("constraint operator", o, e)?,
                        };
                        Ok(ast::WhereConstraintHint(l, o, p_hint(&c.right_type, e)?))
                    }
                    _ => missing_syntax("where constraint", n, e),
                }
            };
            c.constraints
                .syntax_node_to_list_skip_separator()
                .map(|n| f(n, env))
                .collect()
        }
        _ => missing_syntax(
            format!("{} constraints", parent_kind.as_str()).as_str(),
            parent,
            env,
        ),
    }
}

fn p_namespace_use_kind<'a>(kind: S<'a>, env: &mut Env<'a>) -> Result<ast::NsKind> {
    use ast::NsKind::*;
    match &kind.children {
        Missing => Ok(NSClassAndNamespace),
        _ => match token_kind(kind) {
            Some(TK::Namespace) => Ok(NSNamespace),
            Some(TK::Type) => Ok(NSClass),
            Some(TK::Function) => Ok(NSFun),
            Some(TK::Const) => Ok(NSConst),
            _ => missing_syntax("namespace use kind", kind, env),
        },
    }
}

fn p_namespace_use_clause<'a>(
    prefix: Option<S<'a>>,
    kind: Result<ast::NsKind>,
    node: S<'a>,
    env: &mut Env<'a>,
) -> Result<(ast::NsKind, ast::Sid, ast::Sid)> {
    static NAMESPACE_USE: LazyLock<regex::Regex> =
        LazyLock::new(|| regex::Regex::new("[^\\\\]*$").unwrap());

    match &node.children {
        NamespaceUseClause(NamespaceUseClauseChildren {
            clause_kind,
            alias,
            name,
            ..
        }) => {
            let ast::Id(p, n) = match (prefix, pos_name(name, env)?) {
                (None, id) => id,
                (Some(prefix), ast::Id(p, n)) => ast::Id(p, pos_name(prefix, env)?.1 + &n),
            };
            let alias = if alias.is_missing() {
                let x = NAMESPACE_USE.find(&n).unwrap().as_str();
                ast::Id(p.clone(), x.to_string())
            } else {
                let result = pos_name(alias, env)?;
                if result.1.contains(':') || result.1.contains('-') {
                    raise_parsing_error(alias, env, &syntax_error::invalid_namespace_alias);
                }
                result
            };
            let kind = if clause_kind.is_missing() {
                kind
            } else {
                p_namespace_use_kind(clause_kind, env)
            }?;
            if n.contains(':') || n.contains('-') {
                raise_parsing_error(node, env, &syntax_error::invalid_namespace_name);
            }
            Ok((
                kind,
                ast::Id(
                    p,
                    if !n.is_empty() && n.starts_with('\\') {
                        n
                    } else {
                        String::from("\\") + &n
                    },
                ),
                alias,
            ))
        }
        _ => missing_syntax("namespace use clause", node, env),
    }
}

fn is_memoize_attribute_with_flavor(u: &aast::UserAttribute<(), ()>, flavor: Option<&str>) -> bool {
    sn::user_attributes::is_memoized(&u.name.1)
        && (match flavor {
            Some(flavor) => u.params.iter().any(
                |p| matches!(p, Expr(_, _, ast::Expr_::EnumClassLabel(ecl)) if ecl.1 == flavor),
            ),
            None => u.params.is_empty(),
        })
}

fn populate_memoize_default<'a>(
    contexts: Option<&ast::Contexts>,
    user_attributes: &mut aast::UserAttributes<(), ()>,
    env: &mut Env<'a>,
) {
    // If TreatNonAnnotatedMemoizeAsKBIC was supplied by config, add that to empty __Memoize
    if env.parser_options.treat_non_annotated_memoize_as_kbic {
        if let Some(u) = user_attributes
            .iter_mut()
            .find(|u| sn::user_attributes::is_memoized(&u.name.1))
        {
            if u.params.is_empty() && !contexts_cannot_access_ic(contexts) {
                u.params.push(ast::Expr::new(
                    (),
                    env.mk_none_pos(),
                    ast::Expr_::EnumClassLabel(Box::new((
                        None,
                        sn::memoize_option::KEYED_BY_IC.to_string(),
                    ))),
                ));
            }
        }
    }
}

fn check_effect_memoized<'a>(
    contexts: Option<&ast::Contexts>,
    user_attributes: &[aast::UserAttribute<(), ()>],
    kind: &str,
    env: &mut Env<'a>,
) {
    // functions with dependent contexts cannot be memoized
    if has_polymorphic_context(env, contexts) {
        if let Some(u) = user_attributes
            .iter()
            .find(|u| sn::user_attributes::is_memoized(&u.name.1))
        {
            raise_parsing_error_pos(
                &u.name.0,
                env,
                &syntax_error::effect_polymorphic_memoized(kind),
            )
        }
    }
    // memoized functions with zoned must be #KeyedByIC
    if has_any_policied_context(contexts) {
        if let Some(u) = user_attributes
            .iter()
            .find(|u| is_memoize_attribute_with_flavor(u, None))
        {
            raise_parsing_error_pos(
                &u.name.0,
                env,
                &syntax_error::effect_policied_memoized(kind),
            )
        }
    }
    // #KeyedByIC can only be used on functions with defaults or zoned*
    if let Some(u) = user_attributes
        .iter()
        .find(|u| is_memoize_attribute_with_flavor(u, Some(sn::memoize_option::KEYED_BY_IC)))
    {
        if !has_any_policied_or_defaults_context(contexts) {
            raise_parsing_error_pos(
                &u.name.0,
                env,
                &syntax_error::policy_sharded_memoized_without_policied(kind),
            )
        }
    }

    if let Some(u) = user_attributes
        .iter()
        .find(|u| sn::user_attributes::is_memoized(&u.name.1))
    {
        if contexts_cannot_access_ic(contexts) {
            // functions whose contexts prevent getting the IC (effectively <= [leak_safe, globals])
            // cannot pass a memoize argument
            if !u.params.is_empty() {
                raise_parsing_error_pos(
                    &u.name.0,
                    env,
                    &syntax_error::memoize_category_without_implicit_policy_capability(kind),
                )
            }
        } else if env.parser_options.disallow_non_annotated_memoize
            && !env.parser_options.treat_non_annotated_memoize_as_kbic
            && u.params.is_empty()
            && env.file_mode() != file_info::Mode::Mhhi
        {
            // if DisallowNonAnnotatedMemoize was supplied by config and coeffects allow
            // <<__Memoize>> to be categorized, then plain <<__Memoize>> are not allowed.
            // This only matters if TreatNonAnnotatedMemoizeAsKBIC was not supplied
            raise_parsing_error_pos(
                &u.name.0,
                env,
                &syntax_error::memoize_without_annotation_disabled(&u.name.1),
            );
        }
    }
}

fn check_context_has_this<'a>(contexts: Option<&ast::Contexts>, env: &mut Env<'a>) {
    use ast::Hint_::Haccess;
    use ast::Hint_::Happly;
    if let Some(ast::Contexts(pos, context_hints)) = contexts {
        context_hints.iter().for_each(|c| match *c.1 {
            Haccess(ref root, _) => match &*root.1 {
                Happly(oxidized::ast::Id(_, id), _)
                    if strip_ns(id.as_str()) == sn::typehints::THIS =>
                {
                    raise_parsing_error_pos(
                        pos,
                        env,
                        "this:: context is not allowed on top level functions",
                    )
                }
                _ => {}
            },
            _ => {}
        });
    }
}

fn check_effect_polymorphic_reification<'a>(
    contexts: Option<&ast::Contexts>,
    env: &mut Env<'a>,
    node: S<'a>,
) {
    use ast::Hint_::Haccess;
    use ast::Hint_::Happly;
    if let Some(ast::Contexts(_, context_hints)) = contexts {
        context_hints.iter().for_each(|c| match *c.1 {
            Haccess(ref root, _) => match &*root.1 {
                Happly(oxidized::ast::Id(_, id), _) => {
                    fail_if_invalid_reified_generic(node, env, strip_ns(id.as_str()))
                }
                _ => {}
            },
            _ => {}
        });
    }
}

fn check_asio_lowpri<'a>(
    fun_kind: ast::FunKind,
    user_attributes: &[aast::UserAttribute<(), ()>],
    env: &mut Env<'a>,
) {
    // asio low pri attributes are only valid on non-generator async functions
    if let Some(u) = user_attributes
        .iter()
        .find(|u| sn::user_attributes::is_asio_low_pri(&u.name.1))
    {
        if fun_kind != ast::FunKind::FAsync {
            raise_parsing_error_pos(&u.name.0, env, &syntax_error::asio_low_pri_check)
        }
    }
}

fn p_const_value<'a>(node: S<'a>, env: &mut Env<'a>, default_pos: Pos) -> Result<ast::Expr> {
    match &node.children {
        SimpleInitializer(c) => p_expr(&c.value, env),
        _ if env.file_mode() == file_info::Mode::Mhhi && !env.is_codegen() => {
            // We use Omitted as a placeholder here because we don't care about
            // the constant's value when in HHI mode
            Ok(Expr::new((), default_pos, Expr_::Omitted))
        }
        _ => missing_syntax("simple initializer", node, env),
    }
}

fn get_current_package<'a>(env: &mut Env<'a>, node: S<'a>) -> Option<PackageMembership> {
    match &env.package {
        PackageMembershipCache::NoPackage => return None,
        PackageMembershipCache::Package(package) => return Some(package.clone()),
        PackageMembershipCache::Uncomputed => {}
    }
    let span = p_pos(node, env);
    let filepath = span.filename().path_str();
    let parser_options = env.parser_options;
    let package = parser_options
        .package_info
        .get_package_for_file(parser_options.package_support_multifile_tests, filepath)
        .map(|p| PackageMembership::PackageConfigAssignment(p.name.1.clone()));
    env.package = match &package {
        Some(package) => PackageMembershipCache::Package(package.clone()),
        None => PackageMembershipCache::NoPackage,
    };
    package
}

/// A file under an implicit package family's `path` must sit inside a member
/// subdirectory; one placed directly under the family path belongs to no member
/// package, so it is illegal.
fn check_implicit_package_placement(env: &mut Env<'_>) {
    use oxidized::package_info_impl::ImplicitFamilyPlacement;

    let parser_options = env.parser_options;
    let file_path = env.source_text().file_path_rc();
    let placement = parser_options.package_info.check_implicit_family_placement(
        parser_options.package_support_multifile_tests,
        file_path.path_str(),
    );
    // Both errors are anchored at a zero-width position at the start of the
    // file: a `.hack` file has no `<?hh` pragma to point at.
    let error = match placement {
        ImplicitFamilyPlacement::Valid => return,
        ImplicitFamilyPlacement::DirectlyUnderFamily(family) => {
            let (family_pos, family_name) = (family.0.clone(), family.1.clone());
            let pos = Pos::from_lnum_bol_offset(file_path, (1, 0, 0), (1, 0, 0));
            Naming::implicit_package_file_directly_under_path(pos, &family_name, family_pos)
        }
        ImplicitFamilyPlacement::InvalidMemberDir(family, member_dir) => {
            let (family_pos, family_name) = (family.0.clone(), family.1.clone());
            let pos = Pos::from_lnum_bol_offset(file_path, (1, 0, 0), (1, 0, 0));
            Naming::implicit_package_invalid_member_dir(pos, &family_name, family_pos, &member_dir)
        }
    };
    raise_hh_error(env, error);
}

fn p_def<'a>(node: S<'a>, env: &mut Env<'a>) -> Result<Vec<ast::Def>> {
    let doc_comment_opt = extract_docblock(node, env);
    match &node.children {
        FunctionDeclaration(FunctionDeclarationChildren {
            attribute_spec,
            declaration_header,
            body,
        }) => {
            let mut env = Env::clone_and_unset_toplevel_if_toplevel(env);
            let env = env.as_mut();
            env.clear_generics();
            let hdr = p_fun_hdr(declaration_header, env)?;
            let is_external = body.is_external();
            let (block, yield_) = if is_external {
                (Default::default(), false)
            } else {
                env.reset_for_function_body(body, hdr.suspension_kind, p_function_body)?
            };
            let fun_kind = mk_fun_kind(hdr.suspension_kind, yield_);
            let mut user_attributes = p_user_attributes(attribute_spec, env);
            populate_memoize_default(hdr.contexts.as_ref(), &mut user_attributes, env);
            check_effect_memoized(hdr.contexts.as_ref(), &user_attributes, "function", env);
            check_context_has_this(hdr.contexts.as_ref(), env);
            check_asio_lowpri(fun_kind, &user_attributes, env);
            let ret = ast::TypeHint((), hdr.return_type);

            let fun = ast::Fun_ {
                span: p_fun_pos(node, env),
                readonly_this: hdr.readonly_this,
                tparams: vec![],
                annotation: (),
                ret,
                readonly_ret: hdr.readonly_return,
                params: hdr.parameters,
                ctxs: hdr.contexts,
                unsafe_ctxs: hdr.unsafe_contexts,
                body: ast::FuncBody { fb_ast: block },
                fun_kind,
                user_attributes,
                external: is_external,
                hidden: false,
                doc_comment: doc_comment_opt,
            };

            Ok(vec![ast::Def::mk_fun(ast::FunDef {
                namespace: mk_empty_ns_env(env),
                file_attributes: vec![],
                mode: env.file_mode(),
                name: hdr.name,
                fun,
                internal: hdr.internal,
                module: None,
                tparams: hdr.type_parameters,
                where_constraints: hdr.constrs,
                package: get_current_package(env, node),
            })])
        }
        ClassAliasDeclaration(c) => {
            let name = pos_name(&c.name, env)?;
            let tparams = p_tparam_l(true, &c.type_parameters, env)?;
            let original = pos_name(&c.original_name, env)?;
            let original_tparams = p_tparam_l(true, &c.original_type_parameters, env)?;
            Ok(vec![ast::Def::mk_class_alias(ast::ClassAlias {
                name,
                tparams,
                original,
                original_tparams,
            })])
        }
        ClassishDeclaration(c) if contains_class_body(c) => {
            let mut env = Env::clone_and_unset_toplevel_if_toplevel(env);
            let env = env.as_mut();
            let mode = env.file_mode();
            let user_attributes = p_user_attributes(&c.attribute, env);
            let docs_url = p_docs_url(&user_attributes, env);

            let kinds = p_kinds(&c.modifiers, env);
            let final_ = kinds.has(modifier::FINAL);
            let is_xhp = matches!(
                token_kind(&c.name),
                Some(TK::XHPElementName) | Some(TK::XHPClassName)
            );
            let has_xhp_keyword = matches!(token_kind(&c.xhp), Some(TK::XHP));
            let name = pos_name(&c.name, env)?;
            env.clear_generics();
            let tparams = p_tparam_l(true, &c.type_parameters, env)?;
            let class_kind = match token_kind(&c.keyword) {
                Some(TK::Class) if kinds.has(modifier::ABSTRACT) => {
                    ast::ClassishKind::Cclass(ast::Abstraction::Abstract)
                }
                Some(TK::Class) => ast::ClassishKind::Cclass(ast::Abstraction::Concrete),
                Some(TK::Interface) => ast::ClassishKind::Cinterface,
                Some(TK::Trait) => ast::ClassishKind::Ctrait,
                Some(TK::Enum) => ast::ClassishKind::Cenum,
                _ => missing_syntax("class kind", &c.keyword, env)?,
            };
            let extends = could_map(&c.extends_list, env, p_hint)?;
            *env.parent_maybe_reified() = match extends.first().map(|h| h.1.as_ref()) {
                Some(ast::Hint_::Happly(_, hl)) => !hl.is_empty(),
                _ => false,
            };
            let implements = could_map(&c.implements_list, env, p_hint)?;
            let namespace = mk_empty_ns_env(env);
            let span = p_pos(node, env);
            let mut class_ = ast::Class_ {
                span,
                annotation: (),
                mode,
                final_,
                is_xhp,
                has_xhp_keyword,
                kind: class_kind,
                name,
                tparams,
                extends,
                uses: vec![],
                xhp_attr_uses: vec![],
                xhp_category: None,
                reqs: vec![],
                implements,
                consts: vec![],
                typeconsts: vec![],
                vars: vec![],
                methods: vec![],
                xhp_children: vec![],
                xhp_attrs: vec![],
                namespace,
                user_attributes,
                file_attributes: vec![],
                enum_: None,
                doc_comment: doc_comment_opt,
                emit_id: None,
                internal: kinds.has(modifier::INTERNAL),
                module: None,
                docs_url,
                package: get_current_package(env, node),
            };
            match &c.body.children {
                ClassishBody(c1) => {
                    for elt in c1.elements.syntax_node_to_list_skip_separator() {
                        p_class_elt(&mut class_, elt, env);
                    }
                }
                _ => missing_syntax("classish body", &c.body, env)?,
            }
            Ok(vec![ast::Def::mk_class(class_)])
        }
        ConstDeclaration(c) => {
            let ty = &c.type_specifier;
            let decls = c.declarators.syntax_node_to_list_skip_separator();
            let mut defs = vec![];
            for decl in decls {
                let def = match &decl.children {
                    ConstantDeclarator(c) => {
                        let name = &c.name;
                        let init = &c.initializer;
                        let gconst = ast::Gconst {
                            annotation: (),
                            mode: env.file_mode(),
                            name: pos_name(name, env)?,
                            type_: map_optional(ty, env, p_hint)?,
                            value: p_const_value(init, env, p_pos(name, env))?,
                            namespace: mk_empty_ns_env(env),
                            span: p_pos(node, env),
                            emit_id: None,
                            module: None,
                            package: get_current_package(env, node),
                        };
                        ast::Def::mk_constant(gconst)
                    }
                    _ => missing_syntax("constant declaration", decl, env)?,
                };
                defs.push(def);
            }
            Ok(defs)
        }
        AliasDeclaration(c) => {
            let tparams = p_tparam_l(false, &c.generic_parameter, env)?;
            let kinds = p_kinds(&c.modifiers, env);
            let is_module_newtype = !c.module_kw_opt.is_missing();
            let vis = match token_kind(&c.keyword) {
                Some(TK::Type) => ast::TypedefVisibility::Transparent,
                Some(TK::Newtype) if is_module_newtype => ast::TypedefVisibility::OpaqueModule,
                Some(TK::Newtype) => ast::TypedefVisibility::Opaque,
                _ => missing_syntax("kind", &c.keyword, env)?,
            };
            for tparam in tparams.iter() {
                if tparam.reified != ast::ReifyKind::Erased {
                    raise_parsing_error(node, env, &syntax_error::invalid_reified)
                }
                if vis == ast::TypedefVisibility::Transparent && !tparam.constraints.is_empty() {
                    raise_parsing_error(
                        node,
                        env,
                        &syntax_error::bound_on_transparent_type_alias_generic,
                    )
                }
            }
            let user_attributes = itertools::concat(
                c.attribute_spec
                    .syntax_node_to_list_skip_separator()
                    .map(|attr| p_user_attribute(attr, env))
                    .collect::<Result<Vec<ast::UserAttributes>, _>>()?,
            );
            let docs_url = p_docs_url(&user_attributes, env);

            let (super_constraints, as_constraints) =
                p_tconstraints_into_lower_and_upper(&c.constraint, env);
            let require_one = &mut |kind: &str, cs: Vec<_>| {
                if cs.len() > 1 {
                    let msg = format!(
                        "Multiple `{}` constraints on an alias are not allowed",
                        kind
                    );
                    raise_parsing_error(node, env, &msg);
                }
                cs.into_iter().next()
            };
            let as_constraint = require_one("as", as_constraints);
            let super_constraint = require_one("super", super_constraints);
            let hint = p_hint(&c.type_, env)?;
            let runtime_type = hint.clone();
            Ok(vec![ast::Def::mk_typedef(ast::Typedef {
                annotation: (),
                name: pos_name(&c.name, env)?,
                tparams,
                as_constraint,
                super_constraint,
                user_attributes,
                file_attributes: vec![],
                namespace: mk_empty_ns_env(env),
                mode: env.file_mode(),
                assignment: oxidized::aast::TypedefAssignment::SimpleTypeDef(
                    oxidized::aast::TypedefVisibilityAndHint { vis, hint },
                ),
                runtime_type,
                span: p_pos(node, env),
                emit_id: None,
                is_ctx: false,
                internal: kinds.has(modifier::INTERNAL),
                module: None,
                docs_url,
                doc_comment: doc_comment_opt,
                package: get_current_package(env, node),
            })])
        }
        CaseTypeDeclaration(c) => {
            let kinds = p_kinds(&c.modifiers, env);

            let tparams = p_tparam_l(false, &c.generic_parameter, env)?;
            for tparam in tparams.iter() {
                if tparam.reified != ast::ReifyKind::Erased {
                    raise_parsing_error(node, env, &syntax_error::invalid_reified)
                }
            }

            let user_attributes = itertools::concat(
                c.attribute_spec
                    .syntax_node_to_list_skip_separator()
                    .map(|attr| p_user_attribute(attr, env))
                    .collect::<Result<Vec<ast::UserAttributes>, _>>()?,
            );
            let docs_url = p_docs_url(&user_attributes, env);

            let expect_hint = |node, env: &mut _| match p_hint(node, env) {
                Ok(hint) => Some(hint),
                Err(e) => {
                    emit_error(e, env);
                    None
                }
            };

            let as_constraints = c
                .bounds
                .syntax_node_to_list_skip_separator()
                .filter_map(|bound| expect_hint(bound, env))
                .collect::<Vec<_>>();

            // lower the nodes into variants (hints and optionally where clauses)
            let mut variants = c
                .variants
                .syntax_node_to_list()
                .filter_map(|variant| {
                    if let CaseTypeVariant(ctv) = &variant.children {
                        match expect_hint(&ctv.type_, env) {
                            Some(hint) => {
                                match p_where_constraint(
                                    WhereConstraintSource::CaseTypeVariant,
                                    node,
                                    &ctv.where_clause,
                                    env,
                                ) {
                                    Ok(where_constraints) => {
                                        Some(oxidized::aast::TypedefCaseTypeVariant {
                                            hint,
                                            where_constraints,
                                        })
                                    }
                                    Err(e) => {
                                        emit_error(e, env);
                                        None
                                    }
                                }
                            }
                            None => None,
                        }
                    } else {
                        None
                    }
                })
                .collect::<Vec<_>>();
            // because we want to be able to structurally know that the variant
            // list is never empty in the AST, we store the variants as a head
            // and tail instead of a single list
            // assuming `variants` is non empty (else it's a parse error):
            // we pop off the front of the list to use as the head (`variant`)
            let variant;
            if !variants.is_empty() {
                variant = variants.remove(0);
                let assignment =
                    oxidized::aast::TypedefAssignment::CaseType(variant.clone(), variants.to_vec());
                let runtime_type = if variants.is_empty() {
                    variant.hint
                } else {
                    let mut hints = variants
                        .into_iter()
                        .map(|v| v.hint.clone())
                        .collect::<Vec<_>>();
                    hints.insert(0, variant.hint);
                    let hint_ = ast::Hint_::Hunion(hints);
                    let pos = p_pos(&c.variants, env);
                    ast::Hint::new(pos, hint_)
                };

                // If there are more than one constraints create an intersection
                let as_constraint = if as_constraints.len() > 1 {
                    let hint_ = ast::Hint_::Hintersection(as_constraints);
                    let pos = p_pos(&c.bounds, env);
                    Some(ast::Hint::new(pos, hint_))
                } else {
                    as_constraints.into_iter().next()
                };

                Ok(vec![ast::Def::mk_typedef(ast::Typedef {
                    annotation: (),
                    name: pos_name(&c.name, env)?,
                    tparams,
                    as_constraint,
                    super_constraint: None,
                    user_attributes,
                    file_attributes: vec![],
                    namespace: mk_empty_ns_env(env),
                    mode: env.file_mode(),
                    assignment,
                    runtime_type,
                    span: p_pos(node, env),
                    emit_id: None,
                    is_ctx: false,
                    internal: kinds.has(modifier::INTERNAL),
                    module: None,
                    docs_url,
                    doc_comment: doc_comment_opt,
                    package: get_current_package(env, node),
                })])
            } else {
                missing_syntax("case type variant", node, env)
            }
        }
        ContextAliasDeclaration(c) => {
            let (super_constraint, as_constraint) = p_ctx_constraints(&c.as_constraint, env)?;

            let pos_name = pos_name(&c.name, env)?;
            if let Some(first_char) = pos_name.1.chars().next() {
                if first_char.is_lowercase() {
                    raise_parsing_error(
                        &c.name,
                        env,
                        &syntax_error::user_ctx_should_be_caps(&pos_name.1),
                    )
                }
            }
            if as_constraint.is_none() {
                raise_parsing_error(
                    &c.name,
                    env,
                    &syntax_error::user_ctx_require_as(&pos_name.1),
                )
            }
            let kind = match p_context_list_to_intersection(
                &c.context,
                env,
                "Context aliases cannot alias polymorphic contexts",
            ) {
                Some(h) => h,
                None => {
                    let pos = pos_name.0.clone();
                    let hint_ =
                        ast::Hint_::Happly(ast::Id(pos.clone(), String::from("defaults")), vec![]);
                    ast::Hint::new(pos, hint_)
                }
            };
            let runtime_type = kind.clone();
            Ok(vec![ast::Def::mk_typedef(ast::Typedef {
                annotation: (),
                name: pos_name,
                tparams: vec![],
                as_constraint,
                super_constraint,
                user_attributes: itertools::concat(
                    c.attribute_spec
                        .syntax_node_to_list_skip_separator()
                        .map(|attr| p_user_attribute(attr, env))
                        .collect::<Result<Vec<ast::UserAttributes>, _>>()?,
                ),
                namespace: mk_empty_ns_env(env),
                mode: env.file_mode(),
                file_attributes: vec![],
                assignment: oxidized::aast::TypedefAssignment::SimpleTypeDef(
                    oxidized::aast::TypedefVisibilityAndHint {
                        vis: ast::TypedefVisibility::Opaque,
                        hint: kind,
                    },
                ),
                runtime_type,
                span: p_pos(node, env),
                emit_id: None,
                is_ctx: true,
                // TODO(T116039119): Populate value with presence of internal attribute
                internal: false,
                module: None,
                docs_url: None,
                doc_comment: doc_comment_opt,
                package: get_current_package(env, node),
            })])
        }
        EnumDeclaration(c) => {
            let span = p_pos(node, env);
            let p_enumerator = |n: S<'a>, e: &mut Env<'a>| -> Result<ast::ClassConst> {
                let doc_comment_opt = extract_docblock(n, e);
                match &n.children {
                    Enumerator(c) => Ok(ast::ClassConst {
                        user_attributes: Default::default(),
                        type_: None,
                        id: pos_name(&c.name, e)?,
                        kind: ast::ClassConstKind::CCConcrete(p_expr(&c.value, e)?),
                        span: span.clone(),
                        doc_comment: doc_comment_opt,
                    }),
                    _ => missing_syntax("enumerator", n, e),
                }
            };
            let kinds = p_kinds(&c.modifiers, env);

            let mut includes = vec![];

            let mut p_enum_use = |n: S<'a>, e: &mut Env<'a>| -> Result<()> {
                match &n.children {
                    EnumUse(c) => {
                        let mut uses = could_map(&c.names, e, p_hint)?;
                        Ok(includes.append(&mut uses))
                    }
                    _ => missing_syntax("enum_use", node, e),
                }
            };

            for elt in c.use_clauses.syntax_node_to_list_skip_separator() {
                p_enum_use(elt, env)?;
            }

            let user_attributes = p_user_attributes(&c.attribute_spec, env);
            let docs_url = p_docs_url(&user_attributes, env);

            Ok(vec![ast::Def::mk_class(ast::Class_ {
                annotation: (),
                mode: env.file_mode(),
                user_attributes,
                file_attributes: vec![],
                final_: false,
                kind: ast::ClassishKind::Cenum,
                is_xhp: false,
                has_xhp_keyword: false,
                name: pos_name(&c.name, env)?,
                tparams: vec![],
                extends: vec![],
                implements: vec![],
                consts: could_map(&c.enumerators, env, p_enumerator)?,
                namespace: mk_empty_ns_env(env),
                span: p_pos(node, env),
                enum_: Some(ast::Enum_ {
                    base: p_hint(&c.base, env)?,
                    constraint: map_optional(&c.type_, env, p_tconstraint_ty)?,
                    includes,
                }),
                doc_comment: doc_comment_opt,
                uses: vec![],
                xhp_attr_uses: vec![],
                xhp_category: None,
                reqs: vec![],
                vars: vec![],
                typeconsts: vec![],
                methods: vec![],
                xhp_children: vec![],
                xhp_attrs: vec![],
                emit_id: None,
                internal: kinds.has(modifier::INTERNAL),
                module: None,
                docs_url,
                package: get_current_package(env, node),
            })])
        }

        EnumClassDeclaration(c) => {
            let name = pos_name(&c.name, env)?;
            // Adding __EnumClass
            let mut user_attributes = p_user_attributes(&c.attribute_spec, env);
            let enum_class_attribute = ast::UserAttribute {
                name: ast::Id(name.0.clone(), special_attrs::ENUM_CLASS.to_string()),
                params: vec![],
            };
            user_attributes.push(enum_class_attribute);

            let docs_url = p_docs_url(&user_attributes, env);

            // During lowering we store the base type as is. It will be updated during
            // the naming phase
            let base_type = p_hint(&c.base, env)?;

            let name_s = name.1.clone(); // TODO: can I avoid this clone ?

            let kinds = p_kinds(&c.modifiers, env);

            let class_kind = if kinds.has(modifier::ABSTRACT) {
                ast::ClassishKind::CenumClass(ast::Abstraction::Abstract)
            } else {
                ast::ClassishKind::CenumClass(ast::Abstraction::Concrete)
            };

            // Helper to build X -> HH\MemberOf<enum_name, X>
            let build_elt = |p: Pos, ty: ast::Hint| -> ast::Hint {
                let enum_name = ast::Id(p.clone(), name_s.clone());
                let enum_class = ast::Hint_::mk_happly(enum_name, vec![]);
                let enum_class = ast::Hint::new(p.clone(), enum_class);
                let elt_id = ast::Id(p.clone(), special_classes::MEMBER_OF.to_string());
                let full_type = ast::Hint_::mk_happly(elt_id, vec![enum_class, ty]);
                ast::Hint::new(p, full_type)
            };

            let extends = could_map(&c.extends_list, env, p_hint)?;

            let mut enum_class = ast::Class_ {
                annotation: (),
                mode: env.file_mode(),
                user_attributes,
                file_attributes: vec![],
                final_: false, // TODO(T77095784): support final EDTs
                kind: class_kind,
                is_xhp: false,
                has_xhp_keyword: false,
                name,
                tparams: vec![],
                extends: extends.clone(),
                implements: vec![],
                consts: vec![],
                namespace: mk_empty_ns_env(env),
                span: p_pos(node, env),
                enum_: Some(ast::Enum_ {
                    base: base_type,
                    constraint: None,
                    includes: extends,
                }),
                doc_comment: doc_comment_opt,
                uses: vec![],
                xhp_attr_uses: vec![],
                xhp_category: None,
                reqs: vec![],
                vars: vec![],
                typeconsts: vec![],
                methods: vec![],
                xhp_children: vec![],
                xhp_attrs: vec![],
                emit_id: None,
                internal: kinds.has(modifier::INTERNAL),
                module: None,
                docs_url,
                package: get_current_package(env, node),
            };

            for n in c.elements.syntax_node_to_list_skip_separator() {
                match &n.children {
                    // TODO(T77095784): check pos and span usage
                    EnumClassEnumerator(c) => {
                        // we turn:
                        // - type name = expression;
                        // into
                        // - const MemberOf<enum_name, type> name = expression
                        let name = pos_name(&c.name, env)?;
                        let pos = &name.0;
                        let kinds = p_kinds(&c.modifiers, env);
                        let has_abstract = kinds.has(modifier::ABSTRACT);
                        let elt_type = p_hint(&c.type_, env)?;
                        let full_type = build_elt(pos.clone(), elt_type);
                        let kind = if has_abstract {
                            ast::ClassConstKind::CCAbstract(None)
                        } else {
                            ast::ClassConstKind::CCConcrete(p_simple_initializer(
                                &c.initializer,
                                env,
                            )?)
                        };
                        let class_const = ast::ClassConst {
                            user_attributes: Default::default(),
                            type_: Some(full_type),
                            id: name,
                            kind,
                            span: p_pos(node, env),
                            doc_comment: None,
                        };
                        enum_class.consts.push(class_const)
                    }
                    TypeConstDeclaration(_) => {
                        let doc_comment_opt = extract_docblock(n, env);
                        p_type_constant(n, doc_comment_opt, env, &mut enum_class)
                    }
                    _ => {
                        let pos = p_pos(n, env);
                        raise_parsing_error_pos(
                            &pos,
                            env,
                            &syntax_error::invalid_enum_class_enumerator,
                        )
                    }
                }
            }
            Ok(vec![ast::Def::mk_class(enum_class)])
        }
        InclusionDirective(c) if env.file_mode() != file_info::Mode::Mhhi || env.is_codegen() => {
            let expr = p_expr(&c.expression, env)?;
            Ok(vec![ast::Def::mk_stmt(ast::Stmt::new(
                p_pos(node, env),
                ast::Stmt_::mk_expr(expr),
            ))])
        }
        NamespaceDeclaration(c) => {
            let name = if let NamespaceDeclarationHeader(h) = &c.header.children {
                &h.name
            } else {
                return missing_syntax("namespace_declaration_header", node, env);
            };
            let defs = match &c.body.children {
                NamespaceBody(c) => {
                    let mut env1 = Env::clone_and_unset_toplevel_if_toplevel(env);
                    let env1 = env1.as_mut();
                    itertools::concat(
                        c.declarations
                            .syntax_node_to_list_skip_separator()
                            .map(|n| p_def(n, env1))
                            .collect::<Result<Vec<Vec<_>>, _>>()?,
                    )
                }
                _ => vec![],
            };
            Ok(vec![ast::Def::mk_namespace(pos_name(name, env)?, defs)])
        }
        NamespaceGroupUseDeclaration(c) => {
            let uses: Result<Vec<_>, _> = c
                .clauses
                .syntax_node_to_list_skip_separator()
                .map(|n| {
                    p_namespace_use_clause(
                        Some(&c.prefix),
                        p_namespace_use_kind(&c.kind, env),
                        n,
                        env,
                    )
                })
                .collect();
            Ok(vec![ast::Def::mk_namespace_use(uses?)])
        }
        NamespaceUseDeclaration(c) => {
            let uses: Result<Vec<_>, _> = c
                .clauses
                .syntax_node_to_list_skip_separator()
                .map(|n| p_namespace_use_clause(None, p_namespace_use_kind(&c.kind, env), n, env))
                .collect();
            Ok(vec![ast::Def::mk_namespace_use(uses?)])
        }
        FileAttributeSpecification(_) => {
            let user_attributes = p_user_attribute(node, env)?;
            for attr in &user_attributes {
                if attr.name.1 == sn::user_attributes::PACKAGE_OVERRIDE {
                    if let [aast::Expr(_, pos, ast::Expr_::String(s))] = attr.params.as_slice() {
                        if let Ok(s) = String::from_utf8(s.to_vec()) {
                            env.package = PackageMembershipCache::Package(
                                PackageMembership::PackageOverride(pos.clone(), s),
                            )
                        }
                    }
                }
            }
            Ok(vec![ast::Def::mk_file_attributes(ast::FileAttribute {
                user_attributes,
                namespace: mk_empty_ns_env(env),
            })])
        }
        ModuleDeclaration(md) => {
            let name = pos_module_name(&md.name, env)?;

            Ok(vec![ast::Def::mk_module(ast::ModuleDef {
                annotation: (),
                name,
                user_attributes: p_user_attributes(&md.attribute_spec, env),
                file_attributes: vec![],
                span: p_pos(node, env),
                mode: env.file_mode(),
                doc_comment: doc_comment_opt,
            })])
        }
        ModuleMembershipDeclaration(mm) => {
            Ok(vec![ast::Def::mk_set_module(pos_name(&mm.name, env)?)])
        }
        _ if env.file_mode() == file_info::Mode::Mhhi => Ok(vec![]),
        _ => Ok(vec![ast::Def::mk_stmt(p_stmt(node, env)?)]),
    }
}

fn insert_default_module_if_missing_module_membership(program: &mut Vec<ast::Def>) {
    use aast::Def;
    use aast::Stmt;
    use aast::Stmt_::Markup;
    let mut has_module = false;
    let mut insert_pos = None;
    for (idx, def) in program.iter().enumerate() {
        match def {
            Def::Stmt(box Stmt(_, Markup(_))) => {
                insert_pos = Some(idx + 1);
            }
            Def::SetModule(_) => {
                has_module = true;
                break;
            }
            Def::NamespaceUse(_) | Def::FileAttributes(_) => continue,
            _ => {
                // we disallow module membership past these defs so
                // stop the search here
                break;
            }
        }
    }
    if !has_module {
        program.insert(
            insert_pos.unwrap_or_default(),
            Def::mk_set_module(Id(Pos::NONE, String::from(special_modules::DEFAULT))),
        );
    }
}

fn post_process<'a>(_env: &mut Env<'a>, program: Vec<ast::Def>, acc: &mut Vec<ast::Def>) {
    use aast::Def;
    use aast::Def::*;
    let mut saw_ns: Option<(ast::Sid, Vec<ast::Def>)> = None;
    for def in program.into_iter() {
        if let Namespace(_) = &def {
            if let Some((n, ns_acc)) = saw_ns {
                acc.push(Def::mk_namespace(n, ns_acc));
                saw_ns = None;
            }
        }

        if let Namespace(ns) = def {
            let (n, defs) = *ns;
            if defs.is_empty() {
                saw_ns = Some((n, vec![]));
            } else {
                let mut acc_ = vec![];
                post_process(_env, defs, &mut acc_);
                acc.push(Def::mk_namespace(n, acc_));
            }

            continue;
        }

        if let Stmt(s) = &def {
            if s.1.is_noop() {
                continue;
            }
        }

        if let Some((_, ns_acc)) = &mut saw_ns {
            ns_acc.push(def);
        } else {
            acc.push(def);
        };
    }
    if let Some((n, defs)) = saw_ns {
        acc.push(Def::mk_namespace(n, defs));
    }
}

fn p_program<'a>(node: S<'a>, env: &mut Env<'a>) -> ast::Program {
    let nodes = node.syntax_node_to_list_skip_separator();
    let mut acc = vec![];
    for n in nodes {
        match &n.children {
            EndOfFile(_) => break,
            _ => match p_def(n, env) {
                Err(e) => {
                    emit_error(e, env);
                }
                Ok(mut def) => acc.append(&mut def),
            },
        }
    }
    let mut program = vec![];
    post_process(env, acc, &mut program);
    if env.is_codegen() {
        insert_default_module_if_missing_module_membership(&mut program);
    }
    ast::Program(program)
}

fn p_script<'a>(node: S<'a>, env: &mut Env<'a>) -> ast::Program {
    // Once per file, so a file declaring nothing is still checked.
    check_implicit_package_placement(env);
    match &node.children {
        Script(c) => p_program(&c.declarations, env),
        _ => {
            raise_missing_syntax("script", node, env);
            ast::Program(vec![])
        }
    }
}

/// Convert the FFP syntax `script` to an AAST.
///
/// If we encounter parse errors, write them to `env`, and return as
/// much of an AAST as we can.
pub fn lower<'a>(env: &mut Env<'a>, script: S<'a>) -> ast::Program {
    p_script(script, env)
}

#[cfg(test)]
mod tests {
    use bstr::BString;

    use super::*;

    // Invariant: rfind scans backward from index `i` (inclusive) and returns the
    // nearest matching byte at or before `i`. The `i >= len` guard yields None
    // instead of clamping to the end, and any match strictly after `i` is ignored.
    #[test]
    fn rfind_backward_scan_and_oob_guard() {
        let cases: &[(&[u8], usize, u8, Option<usize>)] = &[
            // Nearest match strictly before `i`.
            (b"a.b.c", 4, b'.', Some(3)),
            // A match sitting exactly at `i` is included.
            (b"a.b.", 3, b'.', Some(3)),
            // Matches after `i` are ignored; only index 1 is at or before `i`.
            (b"a.b.c", 2, b'.', Some(1)),
            // OOB guard: `i == len` returns None even though b'c' exists at index 2.
            (b"abc", 3, b'c', None),
            // OOB guard: `i` far past the end also returns None (no clamping).
            (b"abc", 100, b'a', None),
            // In bounds, but the byte is absent.
            (b"abc", 2, b'z', None),
            // Empty input: `0 >= 0` trips the guard.
            (b"", 0, b'x', None),
            // Realistic caller shape: locate the newline at or before `i`.
            (b"line1\nline2", 6, b'\n', Some(5)),
        ];
        for (idx, &(s, i, c, expected)) in cases.iter().enumerate() {
            assert_eq!(
                rfind(s, i, c),
                expected,
                "case {idx}: rfind({s:?}, {i}, {c})"
            );
        }
    }

    // Invariant: get_quoted_content greedily captures everything between the first
    // and last double quote (after optional leading whitespace); on any non-match
    // it passes the input through unchanged rather than returning empty.
    #[test]
    fn get_quoted_content_extract_and_passthrough() {
        let cases: &[(&[u8], &[u8])] = &[
            // Basic extraction of the double-quoted body.
            (b"\"hello\"", b"hello"),
            // Leading whitespace before the opening quote is skipped, but
            // whitespace inside the quotes is preserved.
            (b"  \" x \"", b" x "),
            // Greedy: captures across interior quotes up to the final quote.
            (b"\"a\" \"b\"", b"a\" \"b"),
            // Newlines inside the quotes are captured (the pattern allows `\n`).
            (b"\"a\nb\"", b"a\nb"),
            // No quotes at all -> fallback passthrough returns the input verbatim.
            (b"hello", b"hello"),
            // A single (unterminated) quote does not match -> passthrough.
            (b"\"x", b"\"x"),
        ];
        for (idx, &(input, expected)) in cases.iter().enumerate() {
            assert_eq!(
                get_quoted_content(input),
                expected,
                "case {idx}: input {input:?}"
            );
        }
    }

    // Invariant: every maximal run of whitespace bytes collapses to exactly one
    // space, including leading and trailing runs (collapse, not trim); a run of
    // mixed whitespace kinds collapses together and non-whitespace is untouched.
    #[test]
    fn unesc_xhp_collapses_whitespace_runs() {
        let cases: &[(&[u8], &[u8])] = &[
            // A run of spaces collapses to one.
            (b"a    b", b"a b"),
            // Mixed whitespace kinds (space/tab/newline/CR) in one run -> one space.
            (b"a \t\n\r b", b"a b"),
            // A leading run collapses to a single leading space (not trimmed).
            (b"  ab", b" ab"),
            // A trailing run collapses to a single trailing space (not trimmed).
            (b"ab  ", b"ab "),
            // Already-single spaces are preserved as single spaces.
            (b"a b c", b"a b c"),
            // No whitespace -> unchanged.
            (b"abc", b"abc"),
            // All whitespace collapses to exactly one space.
            (b"   ", b" "),
        ];
        for (idx, &(input, expected)) in cases.iter().enumerate() {
            assert_eq!(
                unesc_xhp(input).as_slice(),
                expected,
                "case {idx}: input {input:?}"
            );
        }
    }

    // Invariant: after unescaping, a result equal to the two-byte "''" or "\"\""
    // is normalized to an empty string; anything else -- including a lone quote or
    // an already-empty string -- passes through unchanged.
    #[test]
    fn unesc_dbl_empty_quote_guard_vs_passthrough() {
        let cases: &[(&str, &str)] = &[
            // Guard: a bare pair of single quotes normalizes to empty.
            ("''", ""),
            // Guard: a bare pair of double quotes normalizes to empty.
            ("\"\"", ""),
            // The guard applies to the post-unescape value: `\x22\x22` unescapes
            // to `""`, which the guard then maps to empty.
            ("\\x22\\x22", ""),
            // Passthrough: an ordinary string is returned unescaped, unchanged.
            ("hello", "hello"),
            // A single quote is not the two-byte pair, so it passes through.
            ("'", "'"),
        ];
        for (idx, &(input, expected)) in cases.iter().enumerate() {
            assert_eq!(
                unesc_dbl(input).unwrap(),
                BString::from(expected),
                "case {idx}: input {input:?}"
            );
        }
    }

    // Invariant: drop_prefix removes exactly one leading byte iff the string is
    // non-empty and starts with `prefix`; an interior or absent prefix, or an
    // empty string, leaves the input unchanged.
    #[test]
    fn drop_prefix_strips_single_leading_char() {
        let cases: &[(&str, char, &str)] = &[
            // A leading prefix is dropped.
            ("$foo", '$', "foo"),
            // No leading prefix -> unchanged.
            ("foo", '$', "foo"),
            // Empty string -> unchanged (the is_empty guard).
            ("", '$', ""),
            // Only ONE leading occurrence is removed.
            ("$$foo", '$', "$foo"),
            // A prefix in the interior is not at the start -> unchanged.
            ("f$oo", '$', "f$oo"),
            // Works for other single-byte prefixes (e.g. '%').
            ("%raw", '%', "raw"),
            // A prefix equal to the whole string leaves an empty string.
            ("$", '$', ""),
        ];
        for (idx, &(s, prefix, expected)) in cases.iter().enumerate() {
            assert_eq!(
                drop_prefix(s, prefix),
                expected,
                "case {idx}: drop_prefix({s:?}, {prefix:?})"
            );
        }
    }
}
