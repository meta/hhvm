// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::borrow::Cow;
use std::collections::BTreeMap;
use std::matches;
use std::str::FromStr;

use hash::HashMap;
use hash::HashSet;
use hh_autoimport_rust as hh_autoimport;
use itertools::Itertools;
use naming_special_names_rust as sn;
use oxidized::experimental_features::FeatureName;
use oxidized::namespace_env::Mode;
use oxidized::parser_options::ParserOptions;
use parser_core_types::indexed_source_text::IndexedSourceText;
use parser_core_types::lexable_token::LexableToken;
use parser_core_types::syntax_by_ref::positioned_syntax::PositionedSyntax;
use parser_core_types::syntax_by_ref::positioned_token::PositionedToken;
use parser_core_types::syntax_by_ref::positioned_value::PositionedValue;
use parser_core_types::syntax_by_ref::syntax::Syntax;
use parser_core_types::syntax_by_ref::syntax_variant_generated::ListItemChildren;
use parser_core_types::syntax_by_ref::syntax_variant_generated::SyntaxVariant;
use parser_core_types::syntax_by_ref::syntax_variant_generated::SyntaxVariant::*;
use parser_core_types::syntax_error::Error;
use parser_core_types::syntax_error::ErrorType;
use parser_core_types::syntax_error::LvalRoot;
use parser_core_types::syntax_error::SyntaxError;
use parser_core_types::syntax_error::SyntaxQuickfix;
use parser_core_types::syntax_error::{self as errors};
use parser_core_types::syntax_trait::SyntaxTrait;
use parser_core_types::syntax_tree::SyntaxTree;
use parser_core_types::token_kind::TokenKind;
use strum::IntoEnumIterator;

#[derive(Clone, PartialEq, Debug)]
struct Location {
    start_offset: usize,
    end_offset: usize,
}

#[derive(Clone, PartialEq, Debug)]
enum NameKind {
    NameUse,         // `use` construct
    NameDef,         // definition e.g. `class` or `trait`
    NameImplicitUse, // implicit `use` e.g. HH type in type hint
}

enum LvalType {
    LvalTypeNone,
    LvalTypeNonFinal,
    LvalTypeFinal,
    LvalTypeNonFinalInout,
}

use LvalType::*;

enum BinopAllowsAwaitInPositions {
    BinopAllowAwaitBoth,
    BinopAllowAwaitLeft,
    BinopAllowAwaitRight,
    BinopAllowAwaitNone,
}

use BinopAllowsAwaitInPositions::*;
use NameKind::*;

#[derive(Clone, Debug)]
struct FirstUseOrDef {
    location: Location,
    kind: NameKind,
    name: String,
    global: bool,
}

#[derive(PartialEq)]
enum NamespaceType {
    Unspecified,
    Bracketed(Location),
    Unbracketed(Location),
}

#[derive(Clone, Debug)]
enum PipeVariableState {
    Unavailable,
    Captured,
    Available,
}

use NamespaceType::*;

// TODO: is there a more Rust idiomatic way to write this?
#[derive(Clone, Debug)]
enum Strmap<X> {
    ConstCase(HashMap<String, X>),

    /// Attribute names are type names, but for historical reasons we didn't
    /// treat them as case-insensitive like normal type names. Since we are
    /// migrating everything to case-sensitive, treat attributes specially.
    AttrCase(HashMap<String, X>),

    FuncCase(HashMap<String, X>),
    TypeCase(HashMap<String, X>),
    NsCase(HashMap<String, X>),
}

impl<X> Strmap<X> {
    fn mem(&self, k: &str) -> bool {
        match &self {
            TypeCase(m) | NsCase(m) => m.contains_key(&k.to_ascii_lowercase()),
            FuncCase(m) | ConstCase(m) | AttrCase(m) => m.contains_key(k),
        }
    }

    fn add(&mut self, k: &str, v: X) {
        match self {
            TypeCase(m) | NsCase(m) => m.insert(k.to_ascii_lowercase(), v),
            FuncCase(m) | ConstCase(m) | AttrCase(m) => m.insert(k.to_string(), v),
        };
    }

    fn get(&self, k: &str) -> Option<&X> {
        match &self {
            TypeCase(m) | NsCase(m) => m.get(&k.to_ascii_lowercase()),
            FuncCase(m) | ConstCase(m) | AttrCase(m) => m.get(k),
        }
    }

    fn filter<F>(self, f: F) -> Self
    where
        F: Fn(&X) -> bool,
    {
        match self {
            FuncCase(m) => FuncCase(m.into_iter().filter(|(_, x)| f(x)).collect()),
            TypeCase(m) => TypeCase(m.into_iter().filter(|(_, x)| f(x)).collect()),
            NsCase(m) => NsCase(m.into_iter().filter(|(_, x)| f(x)).collect()),
            ConstCase(m) => ConstCase(m.into_iter().filter(|(_, x)| f(x)).collect()),
            AttrCase(m) => AttrCase(m.into_iter().filter(|(_, x)| f(x)).collect()),
        }
    }
}

use crate::Strmap::*;

fn empty_trait_require_clauses() -> Strmap<TokenKind> {
    TypeCase(HashMap::default())
}

#[derive(Clone, Debug)]
struct UsedNames {
    classes: Strmap<FirstUseOrDef>,    // TypeCase
    namespaces: Strmap<FirstUseOrDef>, // NsCase
    functions: Strmap<FirstUseOrDef>,  // FuncCase
    constants: Strmap<FirstUseOrDef>,  // ConstCase
    attributes: Strmap<FirstUseOrDef>, // AttrCase
}

impl UsedNames {
    fn empty() -> Self {
        Self {
            classes: TypeCase(HashMap::default()),
            namespaces: NsCase(HashMap::default()),
            functions: FuncCase(HashMap::default()),
            constants: ConstCase(HashMap::default()),
            attributes: AttrCase(HashMap::default()),
        }
    }
}

type S<'a> = &'a Syntax<'a, PositionedToken<'a>, PositionedValue<'a>>;

#[derive(Clone)]
struct Context<'a> {
    pub active_classish: Option<S<'a>>,
    pub active_methodish: Option<S<'a>>,
    pub active_callable: Option<S<'a>>,
    pub active_callable_attr_spec: Option<S<'a>>,
    pub active_const: Option<S<'a>>,
    pub active_enum_class: Option<S<'a>>,
    pub active_callable_is_generator: bool,
    pub pipe_variable_state: PipeVariableState,
    active_experimental_features: HashSet<FeatureName>,
}

struct Env<'a, State> {
    parser_options: ParserOptions,
    syntax_tree: &'a SyntaxTree<'a, Syntax<'a, PositionedToken<'a>, PositionedValue<'a>>, State>,
    text: IndexedSourceText<'a>,
    context: Context<'a>,
    hhvm_compat_mode: bool,
    hhi_mode: bool,
    mode: Mode,
    systemlib: bool,
}

impl<'a, State> Env<'a, State> {
    fn is_hhvm_compat(&self) -> bool {
        self.hhvm_compat_mode
    }

    fn is_typechecker(&self) -> bool {
        matches!(self.mode, Mode::ForTypecheck)
    }

    fn is_hhi_mode(&self) -> bool {
        self.hhi_mode
    }

    fn is_systemlib(&self) -> bool {
        self.systemlib
    }
}

const GLOBAL_NAMESPACE_NAME: &str = "\\";

fn combine_names(n1: &str, n2: &str) -> String {
    let has_leading_slash = n2.starts_with('\\');
    let has_trailing_slash = n1.ends_with('\\');
    match (has_leading_slash, has_trailing_slash) {
        (true, true) => n1.to_string() + &n2[1..],
        (false, false) => n1.to_string() + "\\" + n2,
        _ => n1.to_string() + n2,
    }
}

fn make_first_use_or_def(
    is_method: bool,
    kind: NameKind,
    location: Location,
    namespace_name: &str,
    name: &str,
) -> FirstUseOrDef {
    FirstUseOrDef {
        location,
        kind,
        name: combine_names(namespace_name, name),
        global: !is_method && namespace_name == GLOBAL_NAMESPACE_NAME,
    }
}

#[derive(Default, Debug, Clone)]
struct ParserErrorsContext {
    is_in_concurrent_block: bool,
    active_expression_tree: bool,
    expression_tree_depth: i32,
}

struct ParserErrors<'a, State> {
    env: Env<'a, State>,
    errors: Vec<SyntaxError>,
    parents: Vec<S<'a>>,

    trait_require_clauses: Strmap<TokenKind>,
    names: UsedNames,
    // Named (not anonymous) namespaces that the current expression is enclosed within.
    nested_namespaces: Vec<S<'a>>,
    namespace_type: NamespaceType,
    namespace_name: String,
    uses_readonly: bool,
    in_module: bool,
    context: ParserErrorsContext,
}

fn strip_ns(name: &str) -> &str {
    match name.chars().next() {
        Some('\\') => &name[1..],
        _ => name,
    }
}

// test a node is a syntaxlist and that the list contains an element
// satisfying a given predicate
fn list_contains_predicate<P>(p: P, node: S<'_>) -> bool
where
    P: Fn(S<'_>) -> bool,
{
    if let SyntaxList(lst) = &node.children {
        lst.iter().any(p)
    } else {
        false
    }
}

fn modifiers_of_function_decl_header_exn<'a>(node: S<'a>) -> S<'a> {
    match &node.children {
        FunctionDeclarationHeader(x) => &x.modifiers,
        _ => panic!("expected to get FunctionDeclarationHeader"),
    }
}

fn get_modifiers_of_declaration<'a>(node: S<'a>) -> Option<S<'a>> {
    match &node.children {
        MethodishDeclaration(x) => Some(modifiers_of_function_decl_header_exn(
            &x.function_decl_header,
        )),
        FunctionDeclaration(x) => {
            Some(modifiers_of_function_decl_header_exn(&x.declaration_header))
        }
        PropertyDeclaration(x) => Some(&x.modifiers),
        ConstDeclaration(x) => Some(&x.modifiers),
        TypeConstDeclaration(x) => Some(&x.modifiers),
        ClassishDeclaration(x) => Some(&x.modifiers),
        EnumClassDeclaration(x) => Some(&x.modifiers),
        EnumClassEnumerator(x) => Some(&x.modifiers),
        EnumDeclaration(x) => Some(&x.modifiers),
        AliasDeclaration(x) => Some(&x.modifiers),
        CaseTypeDeclaration(x) => Some(&x.modifiers),
        _ => None,
    }
}

fn declaration_is_toplevel<'a>(node: S<'a>) -> bool {
    match &node.children {
        FunctionDeclaration(_)
        | ClassishDeclaration(_)
        | EnumClassDeclaration(_)
        | EnumDeclaration(_)
        | CaseTypeDeclaration(_)
        | AliasDeclaration(_) => true,
        _ => false,
    }
}

// tests whether the node's modifiers contain one that satisfies `p`.
fn has_modifier_helper<P>(p: P, node: S<'_>) -> bool
where
    P: Fn(S<'_>) -> bool,
{
    match get_modifiers_of_declaration(node) {
        Some(x) => list_contains_predicate(p, x),
        None => false,
    }
}

// does the node contain the Abstract keyword in its modifiers
fn has_modifier_abstract(node: S<'_>) -> bool {
    has_modifier_helper(|x| x.is_abstract(), node)
}

// does the node contain the Static keyword in its modifiers
fn has_modifier_static(node: S<'_>) -> bool {
    has_modifier_helper(|x| x.is_static(), node)
}

fn has_modifier_readonly(node: S<'_>) -> bool {
    has_modifier_helper(|x| x.is_readonly(), node)
}

// does the node contain the Private keyword in its modifiers
fn has_modifier_private(node: S<'_>) -> bool {
    has_modifier_helper(|x| x.is_private(), node)
}

fn make_location(s: S<'_>, e: S<'_>) -> Location {
    let start_offset = start_offset(s);
    let end_offset = end_offset(e);
    Location {
        start_offset,
        end_offset,
    }
}

fn make_location_of_node(n: S<'_>) -> Location {
    make_location(n, n)
}

fn start_offset(n: S<'_>) -> usize {
    // TODO: this logic should be moved to SyntaxTrait::position, when implemented
    n.leading_start_offset() + n.leading_width()
}

fn end_offset(n: S<'_>) -> usize {
    // TODO: this logic should be moved to SyntaxTrait::position, when implemented
    let w = n.width();
    n.leading_start_offset() + n.leading_width() + w
}

fn get_short_name_from_qualified_name<'a>(name: &'a str, alias: &'a str) -> &'a str {
    if !alias.is_empty() {
        return alias;
    }
    match name.rfind('\\') {
        Some(i) => &name[i + 1..],
        None => name,
    }
}

// Turns a syntax node into a list of nodes; if it is a separated syntax
// list then the separators are filtered from the resulting list.
fn syntax_to_list<'a>(
    include_separators: bool,
    node: S<'a>,
) -> impl DoubleEndedIterator<Item = S<'a>> {
    use std::iter::empty;
    use std::iter::once;

    use itertools::Either::Left;
    use itertools::Either::Right;
    let on_list_item =
        move |x: &'a ListItemChildren<'_, PositionedToken<'_>, PositionedValue<'_>>| {
            if include_separators {
                vec![&x.item, &x.separator].into_iter()
            } else {
                vec![&x.item].into_iter()
            }
        };
    match &node.children {
        Missing => Left(Left(empty())),
        SyntaxList(s) => Left(Right(s.iter().flat_map(move |x| match &x.children {
            ListItem(x) => Left(on_list_item(x)),
            _ => Right(once(x)),
        }))),
        ListItem(x) => Right(Left(on_list_item(x))),
        _ => Right(Right(once(node))),
    }
}

fn syntax_to_list_no_separators<'a>(node: S<'a>) -> impl DoubleEndedIterator<Item = S<'a>> {
    syntax_to_list(false, node)
}

fn syntax_to_list_with_separators<'a>(node: S<'a>) -> impl DoubleEndedIterator<Item = S<'a>> {
    syntax_to_list(true, node)
}

fn assert_last_in_list<'a, F>(assert_fun: F, node: S<'a>) -> Option<S<'a>>
where
    F: Fn(S<'a>) -> bool,
{
    let mut iter = syntax_to_list_no_separators(node);
    iter.next_back();
    iter.find(|x| assert_fun(x))
}

fn attr_spec_to_node_list<'a>(node: S<'a>) -> impl DoubleEndedIterator<Item = S<'a>> {
    use itertools::Either::Left;
    use itertools::Either::Right;
    let f = |attrs| Left(syntax_to_list_no_separators(attrs));
    match &node.children {
        OldAttributeSpecification(x) => f(&x.attributes),
        FileAttributeSpecification(x) => f(&x.attributes),
        _ => Right(std::iter::empty()),
    }
}

fn attr_constructor_call<'a>(
    node: S<'a>,
) -> &'a SyntaxVariant<'a, PositionedToken<'a>, PositionedValue<'a>> {
    match &node.children {
        ConstructorCall(_) => &node.children,
        _ => &Missing,
    }
}

fn is_decorated_expression<F>(node: S<'_>, f: F) -> bool
where
    F: Fn(S<'_>) -> bool,
{
    match &node.children {
        DecoratedExpression(x) => f(&x.decorator),
        _ => false,
    }
}

fn test_decorated_expression_child<F>(node: S<'_>, f: F) -> bool
where
    F: Fn(S<'_>) -> bool,
{
    match &node.children {
        DecoratedExpression(x) => f(&x.expression),
        _ => false,
    }
}

fn is_splat_expression(node: S<'_>) -> bool {
    is_decorated_expression(node, |x| x.is_ellipsis())
        || test_decorated_expression_child(node, is_splat_expression)
}

fn is_variadic_parameter_declaration(node: S<'_>) -> bool {
    match &node.children {
        ParameterDeclaration(x) => x.ellipsis.is_ellipsis(),
        ClosureParameterTypeSpecifier(x) => {
            x.ellipsis.is_ellipsis() && parameter_modifier(node, TokenKind::Named).is_none()
        }
        _ => false,
    }
}
fn is_readonly_parameter_declaration(node: S<'_>) -> bool {
    parameter_modifier(node, TokenKind::Readonly).is_some()
}
fn is_optional_parameter_declaration(node: S<'_>) -> bool {
    parameter_modifier(node, TokenKind::Optional).is_some()
}
fn is_splat_parameter_declaration(node: S<'_>) -> bool {
    match &node.children {
        ParameterDeclaration(x) => x.pre_ellipsis.is_ellipsis(),
        ClosureParameterTypeSpecifier(x) => x.pre_ellipsis.is_ellipsis(),
        _ => false,
    }
}
fn misplaced_variadic_param<'a>(params: S<'a>) -> Option<S<'a>> {
    // For closure type specifiers, allow named parameters after a positional variadic parameter.
    // Only flag if an UNNAMED variadic parameter is followed by an UNNAMED parameter.
    let param_list: Vec<S<'a>> = syntax_to_list_no_separators(params).collect();
    for (i, param) in param_list.iter().enumerate() {
        if is_variadic_parameter_declaration(*param) {
            // Check if there's any UNNAMED parameter after this variadic param
            for later_param in param_list.iter().skip(i + 1) {
                // If later_param is NOT named, then variadic is misplaced
                let is_named = parameter_modifier(later_param, TokenKind::Named).is_some();
                if !is_named {
                    // Found an unnamed param after variadic param -> error
                    return Some(*param);
                }
                // If param is named, continue checking - named params are allowed after variadic
            }
        }
    }
    None
}

fn is_variadic_named_param(param: S<'_>) -> bool {
    let ellipsis = match &param.children {
        ParameterDeclaration(x) => &x.ellipsis,
        ClosureParameterTypeSpecifier(x) => &x.ellipsis,
        _ => return false,
    };
    parameter_modifier(param, TokenKind::Named).is_some() && ellipsis.is_ellipsis()
}

fn misplaced_variadic_named_param<'a>(params: S<'a>) -> Option<S<'a>> {
    let mut params = syntax_to_list_no_separators(params).peekable();
    while let Some(param) = params.next() {
        if is_variadic_named_param(param) && params.peek().is_some() {
            return Some(param);
        }
    }
    None
}

// A variadic `named` parameter collects arguments nothing can refer to, so it
// has no name. Return the name of any that was given one.
fn variadic_named_param_with_name<'a>(params: S<'a>) -> Option<S<'a>> {
    syntax_to_list_no_separators(params).find_map(|param| {
        let (ellipsis, name) = match &param.children {
            ParameterDeclaration(x) => (&x.ellipsis, &x.name),
            ClosureParameterTypeSpecifier(x) => (&x.ellipsis, &x.name),
            _ => return None,
        };
        if parameter_modifier(param, TokenKind::Named).is_some()
            && ellipsis.is_ellipsis()
            && !name.is_missing()
        {
            Some(name)
        } else {
            None
        }
    })
}

// The first variadic `named` parameter collects every named argument that
// matches no named parameter, so a second one can never be reached. Return it.
fn duplicate_variadic_named_param<'a>(params: S<'a>) -> Option<S<'a>> {
    syntax_to_list_no_separators(params)
        .filter(|param| is_variadic_named_param(param))
        .nth(1)
}

fn misplaced_splat_param<'a>(param: S<'a>) -> Option<S<'a>> {
    assert_last_in_list(is_splat_parameter_declaration, param)
}

fn is_named_argument(node: S<'_>) -> bool {
    match &node.children {
        NamedArgument(_) => true,
        _ => false,
    }
}

fn misplaced_splat_arg<'a>(args: S<'a>) -> Option<S<'a>> {
    syntax_to_list_no_separators(args)
        .skip_while(|x| !is_splat_expression(x))
        .skip(1)
        .find(|x| !is_named_argument(x) && !is_splat_expression(x))
}

// If a list ends with a variadic parameter followed by a comma, return it
fn ends_with_variadic_comma<'a>(params: S<'a>) -> Option<S<'a>> {
    let mut iter = syntax_to_list_with_separators(params).rev();
    let y = iter.next();
    let x = iter.next();
    match (x, y) {
        (Some(x), Some(y)) if is_variadic_parameter_declaration(x) && y.is_comma() => Some(y),
        _ => None,
    }
}

// Extract variadic parameter from a parameter list
fn variadic_param<'a>(params: S<'a>) -> Option<S<'a>> {
    syntax_to_list_with_separators(params).find(|&x| is_variadic_parameter_declaration(x))
}

// Extract splat parameter from a parameter list
fn splat_param<'a>(params: S<'a>) -> Option<S<'a>> {
    syntax_to_list_with_separators(params).find(|&x| is_splat_parameter_declaration(x))
}

fn is_parameter_with_default_value(param: S<'_>) -> bool {
    match &param.children {
        ParameterDeclaration(x) => !x.default_value.is_missing(),
        _ => false,
    }
}

fn list_first_duplicate_token<'a>(node: S<'a>) -> Option<S<'a>> {
    if let SyntaxList(lst) = &node.children {
        let mut seen = BTreeMap::new();
        for node in lst.iter().rev() {
            if let Token(t) = &node.children {
                if let Some(dup) = seen.insert(t.kind(), node) {
                    return Some(dup);
                }
            }
        }
    }
    None
}

fn token_kind(node: S<'_>) -> Option<TokenKind> {
    if let Token(t) = &node.children {
        return Some(t.kind());
    }
    None
}

fn parameter_modifier_fields<'a>(param: S<'a>) -> Option<[S<'a>; 4]> {
    match &param.children {
        ParameterDeclaration(x) => Some([&x.optional, &x.call_convention, &x.named, &x.readonly]),
        ClosureParameterTypeSpecifier(x) => {
            Some([&x.optional, &x.call_convention, &x.named, &x.readonly])
        }
        _ => None,
    }
}

fn parameter_modifiers<'a>(param: S<'a>) -> Vec<S<'a>> {
    parameter_modifier_fields(param)
        .into_iter()
        .flatten()
        .filter(|node| !node.is_missing())
        .collect()
}

fn parameter_modifier<'a>(param: S<'a>, kind: TokenKind) -> Option<S<'a>> {
    parameter_modifier_fields(param)?
        .into_iter()
        .find(|node| token_kind(node) == Some(kind))
}

// TODO(T294006289): remove this check once hackc and HHVM support mixed inout/named calls.
fn find_named_modifier_in_param_list_containing_inout<'a>(params: S<'a>) -> Option<S<'a>> {
    let mut first_named = None;
    let mut has_inout = false;

    for param in syntax_to_list_no_separators(params) {
        let named = parameter_modifier(param, TokenKind::Named);
        let inout = parameter_modifier(param, TokenKind::Inout);

        if named.is_some() && inout.is_some() {
            // We separately already report a diagnostic for named inout parameters
            return None;
        }

        first_named = first_named.or(named);
        has_inout |= inout.is_some();
    }

    if has_inout { first_named } else { None }
}

fn parameter_modifier_rank(kind: TokenKind) -> usize {
    match kind {
        TokenKind::Optional => 0,
        TokenKind::Inout => 1,
        TokenKind::Named => 2,
        TokenKind::Readonly => 3,
        _ => unreachable!(),
    }
}

fn duplicate_parameter_modifier_error<'a>(modifiers: &[S<'a>]) -> Option<(S<'a>, Error)> {
    let mut seen = [false; 4];
    for modifier in modifiers {
        let Some(kind) = token_kind(modifier) else {
            continue;
        };
        let rank = parameter_modifier_rank(kind);
        if seen[rank] {
            return Some((
                modifier,
                errors::duplicate_parameter_modifier(kind.to_string()),
            ));
        }
        seen[rank] = true;
    }
    None
}

/**
 * inout+optional are incompatible
 * inout+readonly are incompatible
 * inout+named are incompatible
 */
fn parameter_modifier_incompatibility_errors<'a>(param: S<'a>) -> Vec<(S<'a>, Error)> {
    let optional = parameter_modifier(param, TokenKind::Optional);
    let inout = parameter_modifier(param, TokenKind::Inout);
    let named = parameter_modifier(param, TokenKind::Named);
    let readonly = parameter_modifier(param, TokenKind::Readonly);
    let mut incompatibilities = vec![];
    if optional.is_some()
        && let Some(inout) = inout
    {
        let error = errors::incompatible_parameter_modifiers("optional", "inout");
        let (node, error) = match &param.children {
            ParameterDeclaration(_) => (param, error),
            ClosureParameterTypeSpecifier(_) => (inout, error),
            _ => unreachable!(),
        };
        incompatibilities.push((node, error));
    }
    if inout.is_some()
        && readonly.is_some()
        && let ParameterDeclaration(x) = &param.children
    {
        let node = if x.name.is_missing() {
            &x.ellipsis
        } else {
            &x.name
        };
        incompatibilities.push((
            node,
            errors::incompatible_parameter_modifiers("inout", "readonly"),
        ));
    }
    if inout.is_some()
        && let Some(named) = named
    {
        incompatibilities.push((
            named,
            errors::incompatible_parameter_modifiers("inout", "named"),
        ));
    }
    incompatibilities
}

fn parameter_modifier_order_error<'a>(modifiers: &[S<'a>]) -> Option<(S<'a>, Error)> {
    let Some(out_of_order) = modifiers.windows(2).find_map(|pair| {
        let previous = token_kind(pair[0])?;
        let current = token_kind(pair[1])?;
        (parameter_modifier_rank(previous) >= parameter_modifier_rank(current)).then_some(pair[1])
    }) else {
        return None;
    };
    let correct_order = modifiers
        .iter()
        .filter_map(|modifier| token_kind(modifier))
        .sorted_by_key(|kind| parameter_modifier_rank(*kind))
        .map(|kind| kind.to_string())
        .join(" ");
    Some((
        out_of_order,
        errors::parameter_modifier_order(&correct_order),
    ))
}

fn parameter_modifier_errors<'a>(param: S<'a>) -> Vec<(S<'a>, Error)> {
    let modifiers = parameter_modifiers(param);
    if let Some(error) = duplicate_parameter_modifier_error(&modifiers) {
        return vec![error];
    }

    let incompatibilities = parameter_modifier_incompatibility_errors(param);
    if !incompatibilities.is_empty() {
        return incompatibilities;
    }

    parameter_modifier_order_error(&modifiers)
        .into_iter()
        .collect()
}

// Helper function for common code pattern
fn is_token_kind(node: S<'_>, kind: TokenKind) -> bool {
    token_kind(node) == Some(kind)
}

fn get_modifier_final<'a>(modifiers: S<'a>) -> Option<S<'a>> {
    syntax_to_list_no_separators(modifiers).find(|x| x.is_final())
}

fn is_visibility(x: S<'_>) -> bool {
    x.is_public() || x.is_private() || x.is_protected() || x.is_internal()
}

fn contains_async_not_last(mods: S<'_>) -> bool {
    let mut mod_list = syntax_to_list_no_separators(mods);
    match mod_list.next_back() {
        Some(x) if x.is_async() => false,
        _ => mod_list.any(|x| x.is_async()),
    }
}

fn promoted_params<'a>(
    params: impl DoubleEndedIterator<Item = S<'a>>,
) -> impl DoubleEndedIterator<Item = S<'a>> {
    params.filter(|node| match &node.children {
        ParameterDeclaration(x) => !x.visibility.is_missing(),
        _ => false,
    })
}

// Given a function declaration header, confirm that it is NOT a constructor
// and that the header containing it has visibility modifiers in parameters
fn class_non_constructor_has_visibility_param(node: S<'_>) -> bool {
    match &node.children {
        FunctionDeclarationHeader(node) => {
            let params = syntax_to_list_no_separators(&node.parameter_list);
            (!&node.name.is_construct()) && promoted_params(params).next().is_some()
        }
        _ => false,
    }
}

fn class_constructor_has_tparams(node: S<'_>) -> bool {
    match &node.children {
        FunctionDeclarationHeader(node) => {
            node.name.is_construct() && !node.type_parameter_list.is_missing()
        }
        _ => false,
    }
}

// Ban parameter promotion in abstract constructors.
fn abstract_class_constructor_has_visibility_param(node: S<'_>) -> bool {
    match &node.children {
        FunctionDeclarationHeader(node) => {
            let label = &node.name;
            let params = syntax_to_list_no_separators(&node.parameter_list);
            label.is_construct()
                && list_contains_predicate(|x| x.is_abstract(), &node.modifiers)
                && promoted_params(params).next().is_some()
        }
        _ => false,
    }
}

// whether a function decl has body
fn function_declaration_is_external(node: S<'_>) -> bool {
    match &node.children {
        FunctionDeclaration(syntax) => syntax.body.is_external(),
        _ => false,
    }
}

// whether a methodish decl has body
fn methodish_has_body(node: S<'_>) -> bool {
    match &node.children {
        MethodishDeclaration(syntax) => !syntax.function_body.is_missing(),
        _ => false,
    }
}

// Test whether node is a method that is both abstract and private
fn methodish_abstract_conflict_with_private(node: S<'_>) -> bool {
    let is_abstract = has_modifier_abstract(node);
    let has_private = has_modifier_private(node);
    is_abstract && has_private
}

fn make_error_from_nodes(
    child: Option<SyntaxError>,
    start_node: S<'_>,
    end_node: S<'_>,
    error_type: ErrorType,
    error: errors::Error,
) -> SyntaxError {
    let s = start_offset(start_node);
    let e = end_offset(end_node);
    SyntaxError::make_with_child_and_type(child, s, e, error_type, error, vec![])
}

fn make_error_from_node(node: S<'_>, error: errors::Error) -> SyntaxError {
    make_error_from_nodes(None, node, node, ErrorType::ParseError, error)
}

fn make_error_from_node_with_quickfix(
    node: S<'_>,
    error: errors::Error,
    quickfix_title: &str,
    quickfix_start: usize,
    quickfix_end: usize,
    new_text: &str,
) -> SyntaxError {
    let s = start_offset(node);
    let e = end_offset(node);
    let quickfixes = vec![SyntaxQuickfix {
        title: quickfix_title.into(),
        edits: vec![(quickfix_start, quickfix_end, new_text.into())],
    }];
    SyntaxError::make_with_child_and_type(None, s, e, ErrorType::ParseError, error, quickfixes)
}

fn make_error_from_node_with_type(
    node: S<'_>,
    error: errors::Error,
    error_type: ErrorType,
) -> SyntaxError {
    make_error_from_nodes(None, node, node, error_type, error)
}

fn is_invalid_xhp_attr_enum_item_literal(literal_expression: S<'_>) -> bool {
    if let Token(t) = &literal_expression.children {
        match t.kind() {
            TokenKind::DecimalLiteral
            | TokenKind::SingleQuotedStringLiteral
            | TokenKind::DoubleQuotedStringLiteral => false,
            _ => true,
        }
    } else {
        true
    }
}

fn is_invalid_xhp_attr_enum_item(node: S<'_>) -> bool {
    if let LiteralExpression(x) = &node.children {
        is_invalid_xhp_attr_enum_item_literal(&x.expression)
    } else {
        true
    }
}

fn cant_be_reserved_type_name(name: &str) -> bool {
    // Keep in sync with test/reserved
    match name.to_ascii_lowercase().as_ref() {
        // reserved_global_name
        "callable" | "parent" | "self" => true,
        // reserved_hh_name
        "arraykey" | "bool" | "dynamic" | "float" | "int" | "mixed" | "nonnull" | "noreturn"
        | "nothing" | "null" | "num" | "resource" | "this" | "void" | "_" => true,
        // misc
        "darray" | "false" | "static" | "true" | "varray" => true,
        _ => false,
    }
}

// Given a declaration node, returns the modifier node matching the given
// predicate from its list of modifiers, or None if there isn't one.
fn extract_keyword<'a, F>(modifier: F, declaration_node: S<'a>) -> Option<S<'a>>
where
    F: Fn(S<'a>) -> bool,
{
    get_modifiers_of_declaration(declaration_node).and_then(|modifiers_list| {
        syntax_to_list_no_separators(modifiers_list).find(|x: &S<'a>| modifier(x))
    })
}

// Wrapper function that uses above extract_keyword function to test if node
// contains is_abstract keyword
fn is_abstract_declaration(declaration_node: S<'_>) -> bool {
    extract_keyword(|x| x.is_abstract(), declaration_node).is_some()
}

fn is_abstract_and_async_method(md_node: S<'_>) -> bool {
    is_abstract_declaration(md_node) && extract_keyword(|x| x.is_async(), md_node).is_some()
}

fn parameter_callconv<'a>(param: S<'a>) -> Option<S<'a>> {
    parameter_modifier(param, TokenKind::Inout)
}

fn is_parameter_with_callconv(param: S<'_>) -> bool {
    parameter_callconv(param).is_some()
}

fn make_name_already_used_error(
    node: S<'_>,
    name: &str,
    short_name: &str,
    original_location: &Location,
    report_error: &dyn Fn(&str, &str) -> Error,
) -> SyntaxError {
    let name = strip_ns(name);
    let original_location_error = SyntaxError::make(
        original_location.start_offset,
        original_location.end_offset,
        errors::original_definition,
        vec![],
    );

    let s = start_offset(node);
    let e = end_offset(node);
    SyntaxError::make_with_child_and_type(
        Some(original_location_error),
        s,
        e,
        ErrorType::ParseError,
        report_error(name, short_name),
        vec![],
    )
}

// Given a node, checks if it is a abstract ConstDeclaration
fn is_abstract_const(declaration: S<'_>) -> bool {
    match &declaration.children {
        ConstDeclaration(_) => has_modifier_abstract(declaration),
        _ => false,
    }
}

// Given a node, checks if it is a concrete ConstDeclaration *)
fn is_concrete_const(declaration: S<'_>) -> bool {
    match &declaration.children {
        ConstDeclaration(_) => !has_modifier_abstract(declaration),
        _ => false,
    }
}

// If a variadic parameter has a default value, return it
fn variadic_param_with_default_value<'a>(params: S<'a>) -> Option<S<'a>> {
    variadic_param(params).filter(|x| is_parameter_with_default_value(x))
}

fn splat_param_with_default_value<'a>(params: S<'a>) -> Option<S<'a>> {
    splat_param(params).filter(|x| is_parameter_with_default_value(x))
}

// If a variadic parameter is marked inout, return it
fn variadic_param_with_callconv<'a>(params: S<'a>) -> Option<S<'a>> {
    variadic_param(params).filter(|x| is_parameter_with_callconv(x))
}

fn splat_param_with_callconv<'a>(params: S<'a>) -> Option<S<'a>> {
    splat_param(params).filter(|x| is_parameter_with_callconv(x))
}

fn splat_param_with_variadic<'a>(params: S<'a>) -> Option<S<'a>> {
    splat_param(params).filter(|x| is_variadic_parameter_declaration(x))
}

fn variadic_param_with_readonly<'a>(params: S<'a>) -> Option<S<'a>> {
    variadic_param(params).filter(|x| is_readonly_parameter_declaration(x))
}

fn splat_param_with_readonly<'a>(params: S<'a>) -> Option<S<'a>> {
    splat_param(params).filter(|x| is_readonly_parameter_declaration(x))
}

fn splat_param_with_optional<'a>(params: S<'a>) -> Option<S<'a>> {
    splat_param(params).filter(|x| is_optional_parameter_declaration(x))
}

// If an inout parameter has a default, return the default
fn param_with_callconv_has_default<'a>(node: S<'a>) -> Option<S<'a>> {
    match &node.children {
        ParameterDeclaration(x)
            if is_parameter_with_callconv(node) && is_parameter_with_default_value(node) =>
        {
            Some(&x.default_value)
        }
        _ => None,
    }
}

fn does_unop_create_write(token_kind: Option<TokenKind>) -> bool {
    token_kind.is_some_and(|x| matches!(x, TokenKind::PlusPlus | TokenKind::MinusMinus))
}

fn does_decorator_create_write(token_kind: Option<TokenKind>) -> bool {
    matches!(token_kind, Some(TokenKind::Inout))
}

fn node_lval_type<'a, 'b>(node: S<'a>, parents: &'b [S<'a>]) -> LvalType {
    let is_in_final_lval_position = |mut node, parents: &[S<'a>]| {
        for &parent in parents.iter().rev() {
            match &parent.children {
                SyntaxList(_) | ListItem(_) => {
                    node = parent;
                    continue;
                }
                ExpressionStatement(_) => return true,
                ForStatement(x)
                    if std::ptr::eq(node, &x.initializer) || std::ptr::eq(node, &x.end_of_loop) =>
                {
                    return true;
                }
                UsingStatementFunctionScoped(x) if std::ptr::eq(node, &x.expression) => {
                    return true;
                }
                UsingStatementBlockScoped(x) if std::ptr::eq(node, &x.expressions) => {
                    return true;
                }
                _ => return false,
            }
        }
        false
    };
    let get_arg_call_node_with_parents = |mut node, parents: &'b [S<'a>]| {
        for i in (0..parents.len()).rev() {
            let parent = parents[i];
            match &parent.children {
                SyntaxList(_) | ListItem(_) => {
                    node = parent;
                    continue;
                }
                FunctionCallExpression(x) if std::ptr::eq(node, &x.argument_list) => {
                    if i == 0 {
                        // probably unreachable, but just in case to avoid crashing on 0-1
                        return Some((parent, &parents[0..0]));
                    }
                    let grandparent = parents.get(i - 1).unwrap();
                    return match &grandparent.children {
                        PrefixUnaryExpression(x)
                            if token_kind(&x.operator) == Some(TokenKind::At) =>
                        {
                            Some((grandparent, &parents[..i - 1]))
                        }
                        _ => Some((parent, &parents[..i])),
                    };
                }
                _ => return None,
            }
        }
        None
    };

    let lval_ness_of_function_arg_for_inout =
        |node, parents| match get_arg_call_node_with_parents(node, parents) {
            None => LvalTypeNone,
            Some((call_node, parents)) => {
                if is_in_final_lval_position(call_node, parents) {
                    return LvalTypeFinal;
                }
                match parents.last() {
                    None => LvalTypeNonFinalInout,
                    Some(parent) => match &parent.children {
                        BinaryExpression(x)
                            if std::ptr::eq(call_node, &x.right_operand)
                                && does_binop_create_write_on_left(token_kind(&x.operator)) =>
                        {
                            if is_in_final_lval_position(parent, &parents[..parents.len() - 1]) {
                                LvalTypeFinal
                            } else {
                                LvalTypeNonFinalInout
                            }
                        }
                        _ => LvalTypeNonFinalInout,
                    },
                }
            }
        };

    let unary_expression_operator = |x: S<'a>| match &x.children {
        PrefixUnaryExpression(x) => &x.operator,
        PostfixUnaryExpression(x) => &x.operator,
        _ => panic!("expected expression operator"),
    };

    let unary_expression_operand = |x: S<'a>| match &x.children {
        PrefixUnaryExpression(x) => &x.operand,
        PostfixUnaryExpression(x) => &x.operand,
        _ => panic!("expected expression operator"),
    };

    if let Some(next_node) = parents.last() {
        let parents = &parents[..parents.len() - 1];
        match &next_node.children {
            DecoratedExpression(x)
                if std::ptr::eq(node, &x.expression)
                    && does_decorator_create_write(token_kind(&x.decorator)) =>
            {
                lval_ness_of_function_arg_for_inout(next_node, parents)
            }
            PrefixUnaryExpression(_) | PostfixUnaryExpression(_)
                if std::ptr::eq(node, unary_expression_operand(next_node))
                    && does_unop_create_write(token_kind(unary_expression_operator(next_node))) =>
            {
                if is_in_final_lval_position(next_node, parents) {
                    LvalTypeFinal
                } else {
                    LvalTypeNonFinal
                }
            }
            BinaryExpression(x)
                if std::ptr::eq(node, &x.left_operand)
                    && does_binop_create_write_on_left(token_kind(&x.operator)) =>
            {
                if is_in_final_lval_position(next_node, parents) {
                    LvalTypeFinal
                } else {
                    LvalTypeNonFinal
                }
            }
            ForeachStatement(x) if std::ptr::eq(node, &x.key) || std::ptr::eq(node, &x.value) => {
                LvalTypeFinal
            }
            _ => LvalTypeNone,
        }
    } else {
        LvalTypeNone
    }
}

fn is_foreach_in_for(for_initializer: S<'_>) -> bool {
    if let Some(Syntax {
        children: ListItem(x),
        ..
    }) = for_initializer.syntax_node_to_list().next()
    {
        x.item.is_as_expression()
    } else {
        false
    }
}

/// Which late-bound class references `check_constant_expression` accepts, most permissive first.
#[derive(Clone, Copy, PartialEq, Eq)]
enum LateBoundRule {
    /// Reject `static`.
    NoStatic,
    /// Reject `static`, plus `self` and `parent` in a trait, which resolve against the using class.
    NoLateBound,
}

fn is_good_scope_resolution_qualifier(node: S<'_>, static_allowed: bool) -> bool {
    match &node.children {
        QualifiedName(_) => true,
        Token(token) => match token.kind() {
            TokenKind::Static => static_allowed,
            TokenKind::XHPClassName
            | TokenKind::Name
            | TokenKind::SelfToken
            | TokenKind::Parent => true,
            _ => false,
        },
        _ => false,
    }
}

fn does_binop_create_write_on_left(token_kind: Option<TokenKind>) -> bool {
    token_kind.is_some_and(|x| match x {
        TokenKind::Equal
        | TokenKind::BarEqual
        | TokenKind::PlusEqual
        | TokenKind::StarEqual
        | TokenKind::StarStarEqual
        | TokenKind::SlashEqual
        | TokenKind::DotEqual
        | TokenKind::MinusEqual
        | TokenKind::PercentEqual
        | TokenKind::CaratEqual
        | TokenKind::AmpersandEqual
        | TokenKind::LessThanLessThanEqual
        | TokenKind::GreaterThanGreaterThanEqual
        | TokenKind::QuestionQuestionEqual => true,
        _ => false,
    })
}

fn get_positions_binop_allows_await(t: S<'_>) -> BinopAllowsAwaitInPositions {
    use TokenKind::*;
    match token_kind(t) {
        None => BinopAllowAwaitNone,
        Some(t) => match t {
            BarBar
            | AmpersandAmpersand
            | QuestionColon
            | QuestionQuestion
            | BarQuestionGreaterThan => BinopAllowAwaitLeft,
            Equal
            | BarEqual
            | PlusEqual
            | StarEqual
            | StarStarEqual
            | SlashEqual
            | DotEqual
            | MinusEqual
            | PercentEqual
            | CaratEqual
            | AmpersandEqual
            | LessThanLessThanEqual
            | GreaterThanGreaterThanEqual => BinopAllowAwaitRight,
            Plus
            | Minus
            | Star
            | Slash
            | StarStar
            | EqualEqualEqual
            | LessThan
            | GreaterThan
            | Percent
            | Dot
            | EqualEqual
            | ExclamationEqual
            | ExclamationEqualEqual
            | LessThanEqual
            | LessThanEqualGreaterThan
            | GreaterThanEqual
            | Ampersand
            | Bar
            | LessThanLessThan
            | GreaterThanGreaterThan
            | Carat
            | BarGreaterThan => BinopAllowAwaitBoth,
            _ => BinopAllowAwaitNone,
        },
    }
}

fn unop_allows_await(t: S<'_>) -> bool {
    use TokenKind::*;
    token_kind(t).is_some_and(|t| match t {
        Exclamation | Tilde | Plus | Minus | At | Clone | Print | Readonly | DotDotDot => true,
        _ => false,
    })
}

fn check_prefix_unary_dollar(node: S<'_>) -> bool {
    match &node.children {
        PrefixUnaryExpression(x) if token_kind(&x.operator) == Some(TokenKind::Dollar) => {
            check_prefix_unary_dollar(&x.operand)
        }
        BracedExpression(_) | SubscriptExpression(_) | VariableExpression(_) => false, // these ones are valid
        LiteralExpression(_) | PipeVariableExpression(_) => false, // these ones get caught later
        _ => true,
    }
}

fn is_method_declaration(node: S<'_>) -> bool {
    if let MethodishDeclaration(_) = &node.children {
        true
    } else {
        false
    }
}

fn is_invalid_group_use_clause(kind: S<'_>, clause: S<'_>) -> bool {
    if let NamespaceUseClause(x) = &clause.children {
        let clause_kind = &x.clause_kind;
        if kind.is_missing() {
            match &clause_kind.children {
                Missing => false,
                Token(token)
                    if token.kind() == TokenKind::Function || token.kind() == TokenKind::Const =>
                {
                    false
                }
                _ => true,
            }
        } else {
            !clause_kind.is_missing()
        }
    } else {
        false
    }
}

fn is_invalid_group_use_prefix(prefix: S<'_>) -> bool {
    !prefix.is_namespace_prefix()
}

/// Do these two nodes represent `parent` and `class` respectively
fn is_parent_class_access<'a>(lhs: S<'a>, rhs: S<'a>) -> bool {
    match (&lhs.children, &rhs.children) {
        (Token(tl), Token(tr)) => tl.kind() == TokenKind::Parent && tr.kind() == TokenKind::Class,
        _ => false,
    }
}

// TODO: why do we need :'a everywhere?
impl<'a, State: 'a + Clone> ParserErrors<'a, State> {
    fn new(env: Env<'a, State>) -> Self {
        Self {
            env,
            errors: vec![],
            parents: vec![],
            names: UsedNames::empty(),
            trait_require_clauses: empty_trait_require_clauses(),
            namespace_name: GLOBAL_NAMESPACE_NAME.to_string(),
            namespace_type: Unspecified,
            nested_namespaces: vec![],
            uses_readonly: false,
            in_module: false,
            context: ParserErrorsContext {
                is_in_concurrent_block: false,
                active_expression_tree: false,
                expression_tree_depth: 0,
            },
        }
    }

    fn text(&self, node: S<'a>) -> &'a str {
        node.extract_text(self.env.syntax_tree.text())
            .expect("<text_extraction_failure>")
    }

    fn enable_experimental_feature(&mut self, node: S<'a>, arg: S<'a>) {
        let error_invalid_argument = |self_: &mut Self, message| {
            self_.errors.push(make_error_from_node(
                arg,
                errors::invalid_use_of_enable_unstable_feature(message),
            ))
        };

        match &arg.children {
            LiteralExpression(x) => {
                let text = escaper::unquote_str(self.text(&x.expression));
                let consider_all_possible_features = matches!(self.env.mode, Mode::ForCodegen)
                    || self.env.is_hhi_mode()
                    || self
                        .env
                        .parser_options
                        .consider_unspecified_experimental_features_released
                    || self
                        .env
                        .parser_options
                        .use_legacy_experimental_feature_config;
                match FeatureName::from_str(text) {
                    Ok(feature)
                        if consider_all_possible_features
                            || self
                                .env
                                .parser_options
                                .experimental_features
                                .contains_key(text) =>
                    {
                        // We will enable a feature if
                        // - we are in hhvm,
                        // - we are enabling an experimental feature that is specified in the configuration,
                        // - we are going to consider all experimental features as OngoingRelease status,
                        // - we are in an .hhi file, or
                        // - we are using the legacy hardcoded approach to experimental feature config,
                        // Otherwise, we are in the typechecker, and the feature wasn't specified in the config, and we
                        // want to signal an error for attempting to enable an invalid experimental feature.
                        feature.enable(
                            &self.env.parser_options,
                            self.env.is_hhi_mode(),
                            &mut self.env.context.active_experimental_features,
                            text,
                            &mut |err| self.errors.push(make_error_from_node(node, err)),
                        )
                    }
                    _ => {
                        let valid_features = if consider_all_possible_features {
                            FeatureName::iter().join("\n\t")
                        } else {
                            self.env
                                .parser_options
                                .experimental_features
                                .keys()
                                .join("\n\t")
                        };
                        error_invalid_argument(
                            self,
                            format!(
                                "there is no feature named {}.\nAvailable features are:\n\t{}",
                                text, valid_features,
                            )
                            .as_str(),
                        )
                    }
                }
            }
            _ => error_invalid_argument(self, "this is not a literal string expression"),
        };
    }

    fn mark_uses_readonly(&mut self) {
        self.uses_readonly = true;
    }

    fn check_can_use_feature_errors(
        &self,
        errors: &mut Vec<SyntaxError>,
        node: S<'a>,
        feature: &FeatureName,
    ) {
        if !feature.can_use(
            &self.env.parser_options,
            &self.env.context.active_experimental_features,
        ) {
            errors.push(make_error_from_node(
                node,
                errors::cannot_use_feature(feature.into()),
            ))
        }
    }

    fn check_can_use_feature(&mut self, node: S<'a>, feature: &FeatureName) {
        let mut errors = std::mem::take(&mut self.errors);
        self.check_can_use_feature_errors(&mut errors, node, feature);
        self.errors = errors;
    }

    // Accept `node` if *any* of `features` is enabled. If none is, report the
    // first (primary) feature as the one that must be enabled. Used where a
    // single syntactic form is admitted by more than one feature (e.g. a
    // type-level shape splat is allowed under either the concrete or the
    // type-parameter feature; a finer semantic check distinguishes them later).
    fn check_can_use_any_feature(&mut self, node: S<'a>, features: &[FeatureName]) {
        let enabled = features.iter().any(|feature| {
            feature.can_use(
                &self.env.parser_options,
                &self.env.context.active_experimental_features,
            )
        });
        if !enabled {
            if let Some(primary) = features.first() {
                self.check_can_use_feature(node, primary);
            }
        }
    }

    fn attr_name(&self, node: S<'a>) -> Option<&'a str> {
        if let ConstructorCall(x) = attr_constructor_call(node) {
            Some(self.text(&x.type_))
        } else {
            None
        }
    }

    fn attr_args(
        &self,
        node: S<'a>,
    ) -> Option<impl DoubleEndedIterator<Item = S<'a>> + use<'a, State>> {
        if let ConstructorCall(x) = attr_constructor_call(node) {
            Some(syntax_to_list_no_separators(&x.argument_list))
        } else {
            None
        }
    }

    fn attribute_specification_contains(&self, node: S<'a>, name: &str) -> bool {
        match &node.children {
            OldAttributeSpecification(_) | FileAttributeSpecification(_) => {
                attr_spec_to_node_list(node).any(|node| self.attr_name(node) == Some(name))
            }
            _ => false,
        }
    }

    fn methodish_contains_attribute(&self, node: S<'a>, attribute: &str) -> bool {
        match &node.children {
            MethodishDeclaration(x) => {
                self.attribute_specification_contains(&x.attribute, attribute)
            }
            _ => false,
        }
    }

    // whether a methodish decl is native
    fn methodish_is_native(&self, node: S<'_>) -> bool {
        self.methodish_contains_attribute(node, sn::user_attributes::NATIVE)
    }

    // By checking the third parent of a methodish node, tests whether the methodish
    // node is inside an interface.
    fn methodish_inside_interface(&self) -> bool {
        self.env
            .context
            .active_classish
            .iter()
            .any(|parent_classish| match &parent_classish.children {
                ClassishDeclaration(cd) => token_kind(&cd.keyword) == Some(TokenKind::Interface),
                _ => false,
            })
    }

    // Test whether node is an external function and not native.
    fn function_declaration_external_not_native(&self, node: S<'_>) -> bool {
        let in_hhi = self.env.is_hhi_mode();
        let is_external = function_declaration_is_external(node);
        let is_native = self.function_declaration_is_native(node);

        !in_hhi && is_external && !is_native
    }

    // Test whether node is a non-abstract method without a body and not native.
    // Here node is the methodish node
    // And methods inside interfaces are inherently considered abstract *)
    fn methodish_non_abstract_without_body_not_native(&self, node: S<'a>) -> bool {
        let non_abstract = !(has_modifier_abstract(node) || self.methodish_inside_interface());
        let not_has_body = !methodish_has_body(node);
        let not_native = !self.methodish_is_native(node);
        let not_hhi = !self.env.is_hhi_mode();

        not_hhi && non_abstract && not_has_body && not_native
    }

    fn active_classish_kind(&self) -> Option<TokenKind> {
        self.env
            .context
            .active_classish
            .and_then(|x| match &x.children {
                ClassishDeclaration(cd) => token_kind(&cd.keyword),
                _ => None,
            })
    }

    fn has_this(&self) -> bool {
        if !self.is_in_active_class_scope() {
            return false;
        }
        match self.env.context.active_methodish {
            Some(x) if has_modifier_static(x) => false,
            _ => true,
        }
    }

    fn is_clone(&self, label: S<'_>) -> bool {
        self.text(label) == sn::members::__CLONE
    }

    fn class_constructor_has_static(&self, node: S<'_>) -> bool {
        self.has_static(node, |x| x.is_construct())
    }

    fn clone_cannot_be_static(&self, node: S<'_>) -> bool {
        self.has_static(node, |x| self.is_clone(x))
    }

    fn has_static<F>(&self, node: S<'_>, f: F) -> bool
    where
        F: Fn(S<'_>) -> bool,
    {
        match &node.children {
            FunctionDeclarationHeader(node) => {
                let label = &node.name;
                f(label)
                    && self
                        .env
                        .context
                        .active_methodish
                        .iter()
                        .any(|&x| has_modifier_static(x))
            }
            _ => false,
        }
    }

    // Ban parameter promotion in interfaces and traits.
    fn interface_or_trait_has_visibility_param(&self, node: S<'_>) -> bool {
        match &node.children {
            FunctionDeclarationHeader(node) => {
                let is_interface_or_trait =
                    self.env
                        .context
                        .active_classish
                        .is_some_and(|parent_classish| match &parent_classish.children {
                            ClassishDeclaration(cd) => {
                                let kind = token_kind(&cd.keyword);
                                kind == Some(TokenKind::Interface) || kind == Some(TokenKind::Trait)
                            }
                            _ => false,
                        });

                let params = syntax_to_list_no_separators(&node.parameter_list);
                promoted_params(params).next().is_some() && is_interface_or_trait
            }
            _ => false,
        }
    }

    fn async_magic_method(&self, node: S<'_>) -> bool {
        match &node.children {
            FunctionDeclarationHeader(node) => {
                // Method names are case sensitive.
                let name = self.text(&node.name);
                match name {
                    sn::members::__DISPOSE_ASYNC => false,
                    _ if sn::members::AS_SET.contains(&name) => {
                        list_contains_predicate(|x| x.is_async(), &node.modifiers)
                    }
                    _ => false,
                }
            }
            _ => false,
        }
    }

    fn reified_parameter_errors(&mut self, node: S<'a>) {
        if let FunctionDeclarationHeader(x) = &node.children {
            if let TypeParameters(x) = &x.type_parameter_list.children {
                self.get_type_params_and_emit_shadowing_errors(&x.parameters);
            }
        }
    }

    fn using_statement_function_scoped_is_legal(&self) -> bool {
        // using is allowed in the toplevel, and also in toplevel async blocks
        let len = self.parents.len();
        if len < 3 {
            return false;
        }
        match (self.parents.get(len - 2), self.parents.get(len - 3)) {
            (
                Some(Syntax {
                    children: CompoundStatement(_),
                    ..
                }),
                Some(x),
            ) => match &x.children {
                FunctionDeclaration(_)
                | MethodishDeclaration(_)
                | AnonymousFunction(_)
                | LambdaExpression(_)
                | AwaitableCreationExpression(_) => true,
                _ => false,
            },
            _ => false,
        }
    }

    // Don't allow a promoted parameter in a constructor if the class
    // already has a property with the same name. Return the clashing name found.
    fn class_constructor_param_promotion_clash(&self, node: S<'a>) -> Option<&str> {
        use itertools::Either::Left;
        use itertools::Either::Right;
        let class_elts = |node: Option<S<'a>>| match node.map(|x| &x.children) {
            Some(ClassishDeclaration(cd)) => match &cd.body.children {
                ClassishBody(cb) => Left(syntax_to_list_no_separators(&cb.elements)),
                _ => Right(std::iter::empty()),
            },
            _ => Right(std::iter::empty()),
        };

        // A property declaration may include multiple property names:
        // public int $x, $y;
        let prop_names = |elt: S<'a>| match &elt.children {
            PropertyDeclaration(x) => Left(
                syntax_to_list_no_separators(&x.declarators).filter_map(|decl| {
                    match &decl.children {
                        PropertyDeclarator(x) => Some(self.text(&x.name)),
                        _ => None,
                    }
                }),
            ),
            _ => Right(std::iter::empty()),
        };

        let param_name = |p: S<'a>| match &p.children {
            ParameterDeclaration(x) => Some(self.text(&x.name)),
            _ => None,
        };

        match &node.children {
            FunctionDeclarationHeader(node) if node.name.is_construct() => {
                let class_var_names: Vec<_> = class_elts(self.env.context.active_classish)
                    .flat_map(prop_names)
                    .collect();
                let params = syntax_to_list_no_separators(&node.parameter_list);
                let mut promoted_param_names = promoted_params(params).filter_map(param_name);
                promoted_param_names.find(|name| class_var_names.contains(name))
            }
            _ => None,
        }
    }

    // check that a constructor is type annotated
    fn class_constructor_has_non_void_type(&self, node: S<'a>) -> bool {
        if !self.env.is_typechecker() {
            false
        } else {
            match &node.children {
                FunctionDeclarationHeader(node) => {
                    let label = &node.name;
                    let type_ano = &node.type_;
                    let function_colon = &node.colon;
                    let is_missing = type_ano.is_missing() && function_colon.is_missing();
                    let is_void = match &type_ano.children {
                        SimpleTypeSpecifier(spec) => spec.specifier.is_void(),
                        _ => false,
                    };
                    label.is_construct() && !(is_missing || is_void)
                }
                _ => false,
            }
        }
    }

    fn unsupported_magic_method_errors(&mut self, node: S<'a>) {
        if let FunctionDeclarationHeader(x) = &node.children {
            let name = self.text(&x.name);
            if sn::members::UNSUPPORTED_SET.contains(&name) {
                self.errors.push(make_error_from_node(
                    node,
                    errors::unsupported_magic_method(name),
                ));
            }
        }
    }

    fn xhp_errors(&mut self, node: S<'a>) {
        match &node.children {
            XHPEnumType(enum_type) => {
                if self.env.is_typechecker() && enum_type.values.is_missing() {
                    self.errors
                        .push(make_error_from_node(&enum_type.values, errors::error2055))
                } else if self.env.is_typechecker() {
                    syntax_to_list_no_separators(&enum_type.values)
                        .filter(|&x| is_invalid_xhp_attr_enum_item(x))
                        .for_each(|item| {
                            self.errors
                                .push(make_error_from_node(item, errors::error2063))
                        })
                }
            }
            XHPExpression(x) => {
                if let XHPOpen(xhp_open) = &x.open.children {
                    if let XHPClose(xhp_close) = &x.close.children {
                        let open_tag = self.text(&xhp_open.name);
                        let close_tag = self.text(&xhp_close.name);
                        if open_tag != close_tag {
                            self.errors.push(make_error_from_node(
                                node,
                                errors::error2070(open_tag, close_tag),
                            ))
                        }
                    }
                }
            }
            _ => {}
        }
    }

    fn invalid_modifier_errors<F>(&mut self, decl_name: &str, node: S<'a>, ok: F)
    where
        F: Fn(TokenKind) -> bool,
    {
        if let Some(modifiers) = get_modifiers_of_declaration(node) {
            let toplevel = declaration_is_toplevel(node);
            for modifier in syntax_to_list_no_separators(modifiers) {
                if let Some(kind) = token_kind(modifier) {
                    if kind == TokenKind::Readonly {
                        self.mark_uses_readonly()
                    }
                    if kind == TokenKind::Internal {
                        if !self.in_module {
                            self.errors.push(make_error_from_node(
                                modifier,
                                errors::internal_outside_of_module,
                            ))
                        }
                    } else if kind == TokenKind::Public && toplevel {
                        if !self.in_module {
                            self.errors.push(make_error_from_node(
                                modifier,
                                errors::public_toplevel_outside_of_module,
                            ))
                        }
                    }
                    if !ok(kind) {
                        self.errors.push(make_error_from_node(
                            modifier,
                            errors::invalid_modifier_for_declaration(
                                decl_name,
                                self.text(modifier),
                            ),
                        ));
                    }
                }
            }
            if let Some(duplicate) = list_first_duplicate_token(modifiers) {
                self.errors.push(make_error_from_node(
                    duplicate,
                    errors::duplicate_modifiers_for_declaration(decl_name),
                ))
            }
            if let SyntaxList(modifiers) = &modifiers.children {
                let modifiers: Vec<S<'a>> = modifiers
                    .iter()
                    .filter(|x: &S<'a>| is_visibility(x))
                    .collect();
                // it is illegal to have multiple visibility modifiers except protected internal or internal protected combinations
                let is_protected_internal = modifiers.len() == 2
                    && ((modifiers[0].is_protected() && modifiers[1].is_internal())
                        || (modifiers[0].is_internal() && modifiers[1].is_protected()));
                if modifiers.len() > 1 && !is_protected_internal {
                    self.errors.push(make_error_from_node(
                        modifiers.last().unwrap(),
                        errors::multiple_visibility_modifiers_for_declaration(decl_name),
                    ));
                }
            }
        }
    }

    // helper since there are so many kinds of errors
    fn produce_error<F, E, X>(
        &mut self,
        check: F,
        node: X,
        error: E, // closure to avoid constant premature concatenation of error strings
        error_node: S<'a>,
    ) where
        F: Fn(&mut Self, X) -> bool,
        E: Fn() -> Error,
    {
        if check(self, node) {
            self.errors.push(make_error_from_node(error_node, error()))
        }
    }

    // helper since there are so many kinds of errors
    fn produce_error_from_check<F, E>(&mut self, check: F, node: S<'a>, error: E)
    where
        F: Fn(S<'a>) -> Option<S<'a>>,
        E: Fn() -> Error,
    {
        if let Some(error_node) = check(node) {
            self.errors.push(make_error_from_node(error_node, error()))
        }
    }

    // Given a function_declaration_header node, returns its function_name
    // as a string opt.
    fn extract_function_name(&self, header_node: S<'a>) -> Option<&'a str> {
        // The '_' arm of this match will never be reached, but the type checker
        // doesn't allow a direct extraction of function_name from
        // function_declaration_header. *)
        match &header_node.children {
            FunctionDeclarationHeader(fdh) => Some(self.text(&fdh.name)),
            _ => None,
        }
    }

    // Return, as a string opt, the name of the function or method given the context *)
    fn first_parent_function_name(&self) -> Option<&str> {
        // Note: matching on either is sound because functions and/or methods cannot be nested

        match self.env.context.active_methodish {
            Some(Syntax {
                children: FunctionDeclaration(x),
                ..
            }) => self.extract_function_name(&x.declaration_header),
            Some(Syntax {
                children: MethodishDeclaration(x),
                ..
            }) => self.extract_function_name(&x.function_decl_header),
            _ => None,
        }
    }

    // Given a particular TokenKind::(Trait/Interface), tests if a given
    // classish_declaration node is both of that kind and declared abstract.
    fn is_classish_kind_declared_abstract(&self, cd_node: S<'a>) -> bool {
        match &cd_node.children {
            ClassishDeclaration(x)
                if is_token_kind(&x.keyword, TokenKind::Trait)
                    || is_token_kind(&x.keyword, TokenKind::Interface) =>
            {
                list_contains_predicate(|x| x.is_abstract(), &x.modifiers)
            }
            _ => false,
        }
    }

    fn is_immediately_in_lambda(&self) -> bool {
        self.env
            .context
            .active_callable
            .is_some_and(|node| match &node.children {
                AnonymousFunction(_) | LambdaExpression(_) | AwaitableCreationExpression(_) => true,
                _ => false,
            })
    }

    // Returns the whether the current context is in an active class scope
    fn is_in_active_class_scope(&self) -> bool {
        self.env.context.active_classish.is_some()
    }

    // Returns the first ClassishDeclaration node or
    // None if there isn't one or classish_kind does not match. *)
    fn first_parent_classish_node(&self, classish_kind: TokenKind) -> Option<S<'a>> {
        self.env
            .context
            .active_classish
            .and_then(|node| match &node.children {
                ClassishDeclaration(cd) if is_token_kind(&cd.keyword, classish_kind) => Some(node),
                _ => None,
            })
    }

    // Return, as a string opt, the name of the closest enclosing classish entity in
    // the given context (not just Classes )
    fn active_classish_name(&self) -> Option<&'a str> {
        self.env.context.active_classish.and_then(|node| {
            if let ClassishDeclaration(cd) = &node.children {
                cd.name.extract_text(self.env.syntax_tree.text())
            } else {
                None
            }
        })
    }

    // Return, as a string opt, the name of the Class in the given context
    fn first_parent_class_name(&self) -> Option<&'a str> {
        self.env
            .context
            .active_classish
            .and_then(|parent_classish| {
                if let ClassishDeclaration(cd) = &parent_classish.children {
                    if token_kind(&cd.keyword) == Some(TokenKind::Class) {
                        return self.active_classish_name();
                    } else {
                        return None; // This arm is never reached
                    }
                }
                None
            })
    }

    // Tests if the immediate classish parent is an interface.
    fn is_inside_interface(&self) -> bool {
        self.first_parent_classish_node(TokenKind::Interface)
            .is_some()
    }

    fn is_interface_and_async_method(&self, md_node: S<'a>) -> bool {
        self.is_inside_interface() && extract_keyword(|x| x.is_async(), md_node).is_some()
    }

    fn get_params_for_enclosing_callable(&self) -> Option<S<'a>> {
        let from_header = |header: S<'a>| match &header.children {
            FunctionDeclarationHeader(fdh) => Some(&fdh.parameter_list),
            _ => None,
        };
        self.env
            .context
            .active_callable
            .and_then(|callable| match &callable.children {
                FunctionDeclaration(x) => from_header(&x.declaration_header),
                MethodishDeclaration(x) => from_header(&x.function_decl_header),
                LambdaExpression(x) => match &x.signature.children {
                    LambdaSignature(x) => Some(&x.parameters),
                    _ => None,
                },
                _ => None,
            })
    }

    fn first_parent_function_attributes_contains(&self, name: &str) -> bool {
        let from_attr_spec = |attr_spec| {
            attr_spec_to_node_list(attr_spec).any(|node| self.attr_name(node) == Some(name))
        };
        match self.env.context.active_methodish {
            Some(Syntax {
                children: FunctionDeclaration(x),
                ..
            }) => from_attr_spec(&x.attribute_spec),
            Some(Syntax {
                children: MethodishDeclaration(x),
                ..
            }) => from_attr_spec(&x.attribute),
            _ => false,
        }
    }

    fn has_inout_params(&self) -> bool {
        self.get_params_for_enclosing_callable()
            .is_some_and(|function_parameter_list| {
                syntax_to_list_no_separators(function_parameter_list)
                    .any(is_parameter_with_callconv)
            })
    }

    fn is_inside_async_method(&self) -> bool {
        let from_header = |header: S<'a>| match &header.children {
            FunctionDeclarationHeader(fdh) => {
                syntax_to_list_no_separators(&fdh.modifiers).any(|x| x.is_async())
            }
            _ => false,
        };
        self.env
            .context
            .active_callable
            .is_some_and(|node| match &node.children {
                FunctionDeclaration(x) => from_header(&x.declaration_header),
                MethodishDeclaration(x) => from_header(&x.function_decl_header),
                AnonymousFunction(x) => !x.async_keyword.is_missing(),
                LambdaExpression(x) => !x.async_.is_missing(),
                AwaitableCreationExpression(_) => true,
                _ => false,
            })
    }

    fn check_type_name_reference(&mut self, name_text: &str, location: Location) {
        if hh_autoimport::is_hh_autoimport(name_text) && !self.names.classes.mem(name_text) {
            let def = make_first_use_or_def(false, NameImplicitUse, location, "HH", name_text);
            self.names.classes.add(name_text, def)
        }
    }

    fn check_type_hint(&mut self, node: S<'a>) {
        for x in node.iter_children() {
            self.check_type_hint(x)
        }
        let check_type_name = |self_: &mut Self, s| {
            self_.check_type_name_reference(self_.text(s), make_location_of_node(node))
        };
        match &node.children {
            SimpleTypeSpecifier(x) => check_type_name(self, &x.specifier),
            GenericTypeSpecifier(x) => check_type_name(self, &x.class_type),
            _ => {}
        }
    }

    // Given a ConstDeclarator node, test whether it is abstract, but has an
    // initializer.
    fn constant_abstract_with_initializer(&self, init: S<'a>) -> bool {
        let is_abstract = match self.env.context.active_const {
            Some(p_const_declaration) if is_abstract_const(p_const_declaration) => true,
            _ => false,
        };
        let has_initializer = !init.is_missing();
        is_abstract && has_initializer
    }

    // Given a ConstDeclarator node, test whether it is concrete, but has no
    // initializer.
    fn constant_concrete_without_initializer(&self, init: S<'a>) -> bool {
        let is_concrete = match self.env.context.active_const {
            Some(p_const_declaration) if is_concrete_const(p_const_declaration) => true,
            _ => false,
        };
        is_concrete && !self.env.is_hhi_mode() && init.is_missing()
    }

    fn methodish_memoize_lsb_on_non_static(&mut self, node: S<'a>) {
        if self.methodish_contains_attribute(node, sn::user_attributes::MEMOIZE_LSB)
            && !has_modifier_static(node)
        {
            self.errors.push(make_error_from_node(
                node,
                errors::memoize_lsb_on_non_static,
            ));
        }
    }

    fn methodish_readonly_check(&mut self, node: S<'a>) {
        if has_modifier_readonly(node) && has_modifier_static(node) {
            self.errors
                .push(make_error_from_node(node, errors::readonly_static_method))
        }
    }

    fn function_declaration_contains_attribute(&self, node: S<'a>, attribute: &str) -> bool {
        match &node.children {
            FunctionDeclaration(x) => {
                self.attribute_specification_contains(&x.attribute_spec, attribute)
            }
            _ => false,
        }
    }

    fn clone_takes_no_arguments(&self, node: S<'_>) -> bool {
        match &node.children {
            FunctionDeclarationHeader(x) => {
                let mut params = syntax_to_list_no_separators(&x.parameter_list);
                self.is_clone(&x.name) && params.next().is_some()
            }
            _ => false,
        }
    }

    fn function_declaration_is_native(&self, node: S<'a>) -> bool {
        self.function_declaration_contains_attribute(node, sn::user_attributes::NATIVE)
    }

    fn methodish_contains_memoize(&self, node: S<'a>) -> bool {
        self.env.is_typechecker()
            && self.is_inside_interface()
            && self.methodish_contains_attribute(node, sn::user_attributes::MEMOIZE)
    }

    fn is_a_literal_expression_of_kind(val: S<'_>, opts: Vec<TokenKind>) -> bool {
        if let LiteralExpression(x) = &val.children
            && let Some(t) = token_kind(&x.expression)
            && opts.contains(&t)
        {
            true
        } else {
            false
        }
    }

    fn check_require_package_arg_is_single_string_literal(&mut self, node: S<'a>) {
        let Some(args) = self.attr_args(node) else {
            // I'm not actually convinced this means 0, but just to be safe
            self.errors.push(make_error_from_node(
                node,
                errors::require_package_wrong_arity(0),
            ));
            return;
        };

        match args.exactly_one() {
            Err(arg_list) => self.errors.push(make_error_from_node(
                node,
                errors::require_package_wrong_arity(arg_list.count()),
            )),
            Ok(arg) => {
                if !Self::is_a_literal_expression_of_kind(
                    arg,
                    vec![
                        TokenKind::SingleQuotedStringLiteral,
                        TokenKind::DoubleQuotedStringLiteral,
                    ],
                ) {
                    self.errors.push(make_error_from_node(
                        arg,
                        errors::invalid_require_package_str_argument(),
                    ))
                }
            }
        }
    }

    fn check_soft_require_package_args(&mut self, node: S<'a>) {
        let Some((mut args, first_arg)) = self.attr_args(node).and_then(|mut args| {
            let first = args.next()?;
            Some((args, first))
        }) else {
            self.errors.push(make_error_from_node(
                node,
                errors::soft_require_package_wrong_arity(0),
            ));
            return;
        };

        if !Self::is_a_literal_expression_of_kind(
            first_arg,
            vec![
                TokenKind::SingleQuotedStringLiteral,
                TokenKind::DoubleQuotedStringLiteral,
            ],
        ) {
            self.errors.push(make_error_from_node(
                first_arg,
                errors::invalid_require_package_str_argument(),
            ))
        }

        let Some(second_arg) = args.next() else {
            // only 1 arg is fine
            return;
        };

        if !Self::is_a_literal_expression_of_kind(second_arg, vec![TokenKind::DecimalLiteral]) {
            self.errors.push(make_error_from_node(
                second_arg,
                errors::invalid_soft_require_package_int_argument(),
            ));
            return;
        }

        let remaining_args = args.count();
        if remaining_args > 0 {
            // only 1 arg is fine
            self.errors.push(make_error_from_node(
                node,
                errors::soft_require_package_wrong_arity(remaining_args + 2),
            ));
        };
    }

    fn check_attrs(&mut self, attrs: S<'a>) {
        for node in attr_spec_to_node_list(attrs) {
            if let Some(n) = self.attr_name(node) {
                if sn::user_attributes::is_simplihack(n) {
                    self.check_can_use_feature(node, &FeatureName::SimpliHack)
                }
                if (sn::user_attributes::ignore_readonly_local_errors(n)
                        || sn::user_attributes::ignore_coeffect_local_errors(n)
                        || sn::user_attributes::is_native(n))
                        && !self.env.is_systemlib()
                        // The typechecker has its own implementation of this that
                        // allows its own testing and better error messaging.
                        // see --tco_is_systemlib
                        && !self.env.is_typechecker()
                {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_attribute_reserved,
                    ));
                }
                if sn::user_attributes::is_require_package(n) {
                    self.check_require_package_arg_is_single_string_literal(node);
                }
                if sn::user_attributes::is_soft_require_package(n) {
                    self.check_soft_require_package_args(node);
                }
                if sn::user_attributes::is_no_disjoint_union(n) {
                    self.check_can_use_feature(node, &FeatureName::NoDisjointUnion);
                }
            }
        }
    }

    fn function_declaration_header_memoize_lsb(&mut self) {
        if let (Some(node), None) = (
            self.env.context.active_methodish,
            self.env.context.active_classish,
        ) {
            // a function, not a method
            if self.function_declaration_contains_attribute(node, sn::user_attributes::MEMOIZE_LSB)
            {
                self.errors.push(make_error_from_node(
                    node,
                    errors::memoize_lsb_on_non_method,
                ))
            }
        }
    }

    fn is_in_enum_class(&self) -> bool {
        let active_classish = match self.env.context.active_classish {
            Some(x) => x,
            _ => return false,
        };
        if let ClassishDeclaration(cd) = &active_classish.children {
            return self.attr_spec_contains_enum_class(&cd.attribute);
        }
        false
    }

    fn is_in_reified_class(&self) -> bool {
        let active_classish = match self.env.context.active_classish {
            Some(x) => x,
            _ => return false,
        };
        if let ClassishDeclaration(x) = &active_classish.children {
            if let TypeParameters(x) = &x.type_parameters.children {
                return syntax_to_list_no_separators(&x.parameters).any(|p| match &p.children {
                    TypeParameter(x) => !x.reified.is_missing(),
                    _ => false,
                });
            }
        };
        false
    }

    fn methodish_errors(&mut self, node: S<'a>) {
        match &node.children {
            FunctionDeclarationHeader(x) => {
                let function_parameter_list = &x.parameter_list;
                let function_type = &x.type_;
                if x.readonly_return.is_readonly() {
                    self.mark_uses_readonly()
                }

                self.produce_error(
                    |self_, x| Self::class_constructor_has_non_void_type(self_, x),
                    node,
                    || errors::error2018,
                    function_type,
                );

                self.produce_error(
                    |_, x| class_non_constructor_has_visibility_param(x),
                    node,
                    || errors::error2010,
                    function_parameter_list,
                );

                self.produce_error(
                    |_, x| class_constructor_has_tparams(x),
                    node,
                    || errors::no_generics_on_constructors,
                    &x.type_parameter_list,
                );

                self.check_type_parameters_attributes(&x.type_parameter_list);

                if let Some(clashing_name) = self.class_constructor_param_promotion_clash(node) {
                    let class_name = self.active_classish_name().unwrap_or("");
                    let error_msg = errors::error2025(class_name, clashing_name);
                    self.errors
                        .push(make_error_from_node(function_parameter_list, error_msg))
                }

                self.produce_error(
                    |_, x| abstract_class_constructor_has_visibility_param(x),
                    node,
                    || errors::error2023,
                    function_parameter_list,
                );

                self.produce_error(
                    |self_, x| Self::interface_or_trait_has_visibility_param(self_, x),
                    node,
                    || errors::error2024,
                    function_parameter_list,
                );
                self.function_declaration_header_memoize_lsb();
            }
            FunctionDeclaration(fd) => {
                let function_attrs = &fd.attribute_spec;
                let body = &fd.body;
                self.check_attrs(function_attrs);

                self.invalid_modifier_errors("Top-level functions", node, |kind| {
                    kind == TokenKind::Async
                        || kind == TokenKind::Internal
                        || kind == TokenKind::Public
                });
                self.produce_error(
                    |self_, x| self_.function_declaration_external_not_native(x),
                    node,
                    || errors::missing_fn_def_body,
                    body,
                );
            }
            MethodishDeclaration(md) => {
                let header_node = &md.function_decl_header;
                let modifiers = modifiers_of_function_decl_header_exn(header_node);
                let class_name = self.active_classish_name().unwrap_or("");
                let method_name = self
                    .extract_function_name(&md.function_decl_header)
                    .unwrap_or("");
                let method_attrs = &md.attribute;
                self.check_attrs(method_attrs);
                self.produce_error(
                    |self_, x| self_.methodish_contains_memoize(x),
                    node,
                    || errors::interface_with_memoize,
                    header_node,
                );
                self.produce_error(
                    |self_, x| self_.class_constructor_has_static(x),
                    header_node,
                    || errors::error2009(class_name, method_name),
                    modifiers,
                );
                self.unsupported_magic_method_errors(header_node);
                self.produce_error(
                    |self_, x| self_.async_magic_method(x),
                    header_node,
                    || errors::async_magic_method(method_name),
                    modifiers,
                );
                self.produce_error(
                    |self_, x| self_.clone_takes_no_arguments(x),
                    header_node,
                    || errors::clone_takes_no_arguments(class_name, method_name),
                    modifiers,
                );
                self.produce_error(
                    |self_, x| self_.clone_cannot_be_static(x),
                    header_node,
                    || errors::clone_cannot_be_static(class_name, method_name),
                    modifiers,
                );
                self.invalid_modifier_errors("Methods", node, |kind| {
                    kind == TokenKind::Abstract
                        || kind == TokenKind::Final
                        || kind == TokenKind::Static
                        || kind == TokenKind::Private
                        || kind == TokenKind::Protected
                        || kind == TokenKind::Internal
                        || kind == TokenKind::Public
                        || kind == TokenKind::Async
                        || kind == TokenKind::Readonly
                });
                if self.is_inside_interface() {
                    self.invalid_modifier_errors("Interface methods", node, |kind| {
                        kind != TokenKind::Final && kind != TokenKind::Abstract
                    });
                };

                let fun_semicolon = &md.semicolon;

                self.produce_error(
                    |self_, x| self_.methodish_non_abstract_without_body_not_native(x),
                    node,
                    || errors::error2015,
                    fun_semicolon,
                );
                self.produce_error(
                    |_, x| methodish_abstract_conflict_with_private(x),
                    node,
                    || errors::error2016(class_name, method_name),
                    modifiers,
                );

                if let Some(modifier) = get_modifier_final(modifiers) {
                    self.produce_error(
                        |_, x| has_modifier_abstract(x),
                        node,
                        || errors::error2019(class_name, method_name),
                        modifier,
                    );
                }
                self.methodish_readonly_check(node);
                self.methodish_memoize_lsb_on_non_static(node);
                let async_annotation = extract_keyword(|x| x.is_async(), node).unwrap_or(node);

                if self.is_interface_and_async_method(node) {
                    let quickfix_start = start_offset(async_annotation);
                    let quickfix_end = end_offset(async_annotation);

                    self.errors.push(make_error_from_node_with_quickfix(
                        async_annotation,
                        errors::error2046("a method in an interface"),
                        "Remove `async`",
                        quickfix_start,
                        quickfix_end,
                        "",
                    ))
                }

                self.produce_error(
                    |_, x| is_abstract_and_async_method(x),
                    node,
                    || errors::error2046("an `abstract` method"),
                    async_annotation,
                );

                if self.env.is_typechecker() {
                    self.produce_error(
                        |_, x| contains_async_not_last(x),
                        modifiers,
                        || errors::async_not_last,
                        modifiers,
                    );
                };
            }
            _ => {}
        }
    }

    fn check_type_parameters_attributes(&mut self, x: S<'a>) {
        if let TypeParameters(x) = &x.children {
            syntax_to_list_no_separators(&x.parameters).for_each(|x| {
                if let TypeParameter(x) = &x.children {
                    let tparam_attributes = &x.attribute_spec;
                    self.check_attrs(tparam_attributes);
                }
            })
        }
    }

    fn is_in_construct_method(&self) -> bool {
        if self.is_immediately_in_lambda() {
            false
        } else {
            self.first_parent_function_name()
                .is_some_and(|s| s == sn::members::__CONSTRUCT)
        }
    }

    fn duplicate_named_param(&self, params: S<'a>) -> Option<S<'a>> {
        let mut names = HashSet::default();
        syntax_to_list_no_separators(params).find_map(|param| {
            // Named variadics have no name; duplicate_variadic_named_param handles them.
            let name = match &param.children {
                ClosureParameterTypeSpecifier(x)
                    if !x.named.is_missing() && !x.ellipsis.is_ellipsis() =>
                {
                    &x.name
                }
                _ => return None,
            };
            let Token(token) = &name.children else {
                return None;
            };
            if names.insert(self.token_text(token)) {
                None
            } else {
                Some(name)
            }
        })
    }

    fn params_errors(&mut self, params: S<'a>) {
        self.produce_error_from_check(ends_with_variadic_comma, params, || errors::error2022);
        self.produce_error_from_check(misplaced_variadic_param, params, || errors::error2021);
        self.produce_error_from_check(misplaced_variadic_named_param, params, || {
            errors::misplaced_variadic_named_param
        });
        self.produce_error_from_check(misplaced_splat_param, params, || errors::error2081);
        self.produce_error_from_check(variadic_named_param_with_name, params, || {
            errors::variadic_named_param_with_name
        });
        self.produce_error_from_check(duplicate_variadic_named_param, params, || {
            errors::duplicate_variadic_named_param
        });
        if let Some(name) = self.duplicate_named_param(params) {
            self.errors
                .push(make_error_from_node(name, errors::duplicate_named_param));
        }
        if let Some(named) = find_named_modifier_in_param_list_containing_inout(params) {
            self.errors.push(make_error_from_node(
                named,
                errors::mixed_inout_and_named_parameters,
            ));
        }

        self.produce_error_from_check(variadic_param_with_default_value, params, || {
            errors::error2065
        });

        self.produce_error_from_check(splat_param_with_default_value, params, || errors::error2082);

        self.produce_error_from_check(variadic_param_with_callconv, params, || errors::error2073);
        self.produce_error_from_check(splat_param_with_callconv, params, || errors::error2083);
        self.produce_error_from_check(splat_param_with_variadic, params, || errors::error2079);
        self.produce_error_from_check(splat_param_with_optional, params, || errors::error2080);
        self.produce_error_from_check(variadic_param_with_readonly, params, || {
            errors::variadic_readonly_param
        });
        self.produce_error_from_check(splat_param_with_readonly, params, || {
            errors::splat_readonly_param
        });
    }

    fn check_parameter_this(&mut self, node: S<'a>) {
        let mut this_param = None;
        if let ParameterDeclaration(p) = &node.children {
            match &p.name.children {
                Token(_) => {
                    // normal parameter $foo
                    if self.text(&p.name) == sn::special_idents::THIS {
                        this_param = Some(&p.name);
                    }
                }
                DecoratedExpression(de) => {
                    // variadic parameter ...$foo
                    if let Token(_) = de.expression.children {
                        if self.text(&de.expression) == sn::special_idents::THIS {
                            this_param = Some(&de.expression);
                        }
                    }
                }
                _ => {}
            }
        }

        if let Some(this_param) = this_param {
            self.errors
                .push(make_error_from_node(this_param, errors::reassign_this));
        }
    }

    fn check_parameter_readonly(&mut self, node: S<'a>) {
        if matches!(&node.children, ParameterDeclaration(_))
            && parameter_modifier(node, TokenKind::Readonly).is_some()
        {
            self.mark_uses_readonly()
        }
    }

    fn lval_errors(&mut self, syntax_node: S<'a>) {
        if let LvalTypeNonFinal = node_lval_type(syntax_node, &self.parents) {
            self.errors.push(make_error_from_node(
                syntax_node,
                errors::lval_as_expression,
            ))
        }
    }

    fn parameter_errors(&mut self, node: S<'a>) {
        for (modifier, error) in parameter_modifier_errors(node) {
            self.errors.push(make_error_from_node(modifier, error));
        }
        let param_errors = |self_: &mut Self, params| {
            for x in syntax_to_list_no_separators(params) {
                self_.check_parameter_this(x);
                self_.check_parameter_readonly(x);
            }
            self_.params_errors(params)
        };
        match &node.children {
            ParameterDeclaration(p) => {
                let callconv_text = self.text(parameter_callconv(node).unwrap_or(node));
                self.produce_error_from_check(param_with_callconv_has_default, node, || {
                    errors::error2074(callconv_text)
                });

                self.check_type_hint(&p.type_);
                self.check_parameter_readonly(node);

                if let Some(inout_modifier) = parameter_callconv(node) {
                    if self.is_inside_async_method() {
                        self.errors.push(make_error_from_node_with_type(
                            inout_modifier,
                            errors::inout_param_in_async,
                            ErrorType::RuntimeError,
                        ))
                    }
                    if self.is_in_construct_method() {
                        self.errors.push(make_error_from_node(
                            inout_modifier,
                            errors::inout_param_in_construct,
                        ))
                    }
                    let in_memoize = self
                        .first_parent_function_attributes_contains(sn::user_attributes::MEMOIZE);
                    let in_memoize_lsb = self.first_parent_function_attributes_contains(
                        sn::user_attributes::MEMOIZE_LSB,
                    );

                    if (in_memoize || in_memoize_lsb) && !self.is_immediately_in_lambda() {
                        self.errors.push(make_error_from_node_with_type(
                            inout_modifier,
                            errors::memoize_with_inout,
                            ErrorType::RuntimeError,
                        ))
                    }
                }
            }
            FunctionDeclarationHeader(x) => param_errors(self, &x.parameter_list),
            AnonymousFunction(x) => param_errors(self, &x.parameters),
            ClosureTypeSpecifier(x) => param_errors(self, &x.parameter_list),
            LambdaExpression(x) => {
                if let LambdaSignature(x) = &x.signature.children {
                    param_errors(self, &x.parameters)
                }
            }
            _ => {}
        }
    }

    // Only check the functions; invalid attributes on methods (like <<__EntryPoint>>)
    // are caught elsewhere
    fn multiple_entrypoint_attribute_errors(&mut self, node: S<'a>) {
        match &node.children {
            FunctionDeclaration(f)
                if self.attribute_specification_contains(
                    &f.attribute_spec,
                    sn::user_attributes::ENTRY_POINT,
                ) =>
            {
                // Get the location of the <<...>> annotation
                let location = match &f.attribute_spec.children {
                    OldAttributeSpecification(x) => make_location_of_node(&x.attributes),
                    _ => panic!("Expected attribute specification node"),
                };
                let def = make_first_use_or_def(
                    false,
                    NameDef,
                    location,
                    &self.namespace_name,
                    sn::user_attributes::ENTRY_POINT,
                );
                match self.names.attributes.get(sn::user_attributes::ENTRY_POINT) {
                    Some(prev_def) => {
                        let (line_num, _) = self
                            .env
                            .text
                            .offset_to_position(prev_def.location.start_offset as isize);

                        let path = self.env.text.source_text().file_path().path_str();
                        let loc = String::from(path) + ":" + &line_num.to_string();
                        let err = errors::multiple_entrypoints(&loc);
                        let err_type = ErrorType::ParseError;
                        self.errors
                            .push(make_error_from_node_with_type(node, err, err_type))
                    }
                    _ => {}
                };
                self.names
                    .attributes
                    .add(sn::user_attributes::ENTRY_POINT, def)
            }
            _ => {}
        }
    }

    fn redeclaration_errors(&mut self, node: S<'a>) {
        match &node.children {
            FunctionDeclarationHeader(f) if !f.name.is_missing() => {
                let mut it = self.parents.iter().rev();
                let p1 = it.next();
                let _ = it.next();
                let p3 = it.next();
                let p4 = it.next();
                match (p1, p3, p4) {
                    (
                        Some(Syntax {
                            children: FunctionDeclaration(_),
                            ..
                        }),
                        Some(Syntax {
                            children: NamespaceBody(_),
                            ..
                        }),
                        _,
                    )
                    | (
                        Some(Syntax {
                            children: FunctionDeclaration(_),
                            ..
                        }),
                        _,
                        None,
                    )
                    | (
                        Some(Syntax {
                            children: MethodishDeclaration(_),
                            ..
                        }),
                        _,
                        _,
                    )
                    | (
                        Some(Syntax {
                            children: MethodishTraitResolution(_),
                            ..
                        }),
                        _,
                        _,
                    ) => {
                        let function_name: &str = self.text(&f.name);
                        let location = make_location_of_node(&f.name);
                        let is_method = match p1 {
                            Some(Syntax {
                                children: MethodishDeclaration(_),
                                ..
                            }) => true,
                            _ => false,
                        };
                        let def = make_first_use_or_def(
                            is_method,
                            NameDef,
                            location,
                            &self.namespace_name,
                            function_name,
                        );
                        match self.names.functions.get(function_name) {
                            Some(prev_def)
                                if prev_def.global == def.global && prev_def.kind == NameDef =>
                            {
                                let (line_num, _) = self
                                    .env
                                    .text
                                    .offset_to_position(prev_def.location.start_offset as isize);

                                let path = self.env.text.source_text().file_path().path_str();
                                let loc = String::from(path) + ":" + &line_num.to_string();
                                let (err, err_type) = match self.first_parent_class_name() {
                                    None => (
                                        errors::redeclaration_of_function(function_name, &loc),
                                        ErrorType::RuntimeError,
                                    ),
                                    Some(class_name) => {
                                        let full_name =
                                            String::from(class_name) + "::" + function_name;
                                        (
                                            errors::redeclaration_of_method(&full_name),
                                            ErrorType::ParseError,
                                        )
                                    }
                                };
                                self.errors
                                    .push(make_error_from_node_with_type(node, err, err_type))
                            }
                            Some(prev_def) if (prev_def.kind != NameDef) => {
                                let (line_num, _) = self
                                    .env
                                    .text
                                    .offset_to_position(prev_def.location.start_offset as isize);
                                let line_num = line_num as usize;

                                self.errors.push(make_name_already_used_error(
                                    &f.name,
                                    &combine_names(&self.namespace_name, function_name),
                                    function_name,
                                    &def.location,
                                    &|x, y| errors::declared_name_is_already_in_use(line_num, x, y),
                                ))
                            }
                            _ => {}
                        };
                        self.names.functions.add(function_name, def)
                    }
                    _ if self.env.is_typechecker() => self.errors.push(make_error_from_node(
                        node,
                        errors::decl_outside_global_scope,
                    )),
                    _ => {}
                }
            }
            _ => {}
        }
    }

    fn statement_errors(&mut self, node: S<'a>) {
        let expect_colon = |colon: S<'a>| match &colon.children {
            Token(m) if self.env.is_typechecker() && m.kind() != TokenKind::Colon => {
                Some((colon, errors::error1020))
            }
            _ => None,
        };
        (match &node.children {
            TryStatement(x) if x.catch_clauses.is_missing() && x.finally_clause.is_missing() => {
                Some((node, errors::error2007))
            }
            UsingStatementFunctionScoped(_) if !self.using_statement_function_scoped_is_legal() => {
                Some((node, errors::using_st_function_scoped_top_level))
            }
            ForStatement(x) if is_foreach_in_for(&x.initializer) => {
                Some((node, errors::for_with_as_expression))
            }
            CaseLabel(x) => expect_colon(&x.colon),

            DefaultLabel(x) => expect_colon(&x.colon),
            _ => None,
        })
        .into_iter()
        .for_each(|(error_node, error_message)| {
            self.errors
                .push(make_error_from_node(error_node, error_message))
        })
    }

    fn invalid_shape_initializer_name(&mut self, node: S<'a>) {
        match &node.children {
            LiteralExpression(x) => {
                let is_str = match token_kind(&x.expression) {
                    Some(TokenKind::SingleQuotedStringLiteral) => true,

                    // TODO: Double quoted string are only legal
                    // if they contain no encapsulated expressions.
                    Some(TokenKind::DoubleQuotedStringLiteral) => true,
                    _ => false,
                };
                if !is_str {
                    self.errors
                        .push(make_error_from_node(node, errors::invalid_shape_field_name))
                }
            }
            NameofExpression(_) | ScopeResolutionExpression(_) => {}
            QualifiedName(_) => {
                if self.env.is_typechecker() {
                    self.errors
                        .push(make_error_from_node(node, errors::invalid_shape_field_name))
                }
            }
            Token(_) if node.is_name() => {
                if self.env.is_typechecker() {
                    self.errors
                        .push(make_error_from_node(node, errors::invalid_shape_field_name))
                }
            }
            _ => self
                .errors
                .push(make_error_from_node(node, errors::invalid_shape_field_name)),
        }
    }

    fn invalid_shape_field_check(&mut self, node: S<'a>, allow_optional: bool) {
        match &node.children {
            FieldInitializer(x) => self.invalid_shape_initializer_name(&x.name),
            VariableExpression(_) => {
                // Shape field punning: $foo becomes 'foo' => $foo
                // Check if the feature is enabled
                self.check_can_use_feature(node, &FeatureName::ShapeFieldPunning);
            }
            PrefixUnaryExpression(c)
                if allow_optional && token_kind(&c.operator) == Some(TokenKind::Question) =>
            {
                self.invalid_shape_field_check(&c.operand, allow_optional)
            }
            ShapeSplatExpression(_) => {
                // Expression-level splat `...$e` inside a `shape(...)` literal.
                self.check_can_use_feature(node, &FeatureName::ShapeSplatExpression);
            }
            _ => self
                .errors
                .push(make_error_from_node(node, errors::invalid_shape_field_name)),
        }
    }

    fn is_in_unyieldable_magic_method(&self) -> bool {
        self.first_parent_function_name().is_some_and(|s| match s {
            sn::members::__INVOKE => false,
            _ => sn::members::AS_SET.contains(&s),
        })
    }

    fn check_disallowed_variables(&mut self, node: S<'a>) {
        match &node.children {
            VariableExpression(x) => {
                // TODO(T75820862): Allow $GLOBALS to be used as a variable name
                let name = self.text(&x.expression);
                if name == sn::superglobals::GLOBALS {
                    self.errors
                        .push(make_error_from_node(node, errors::globals_disallowed))
                } else if sn::superglobals::is_superglobal(name) {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::superglobal_disallowed(name),
                    ))
                } else if name == sn::special_idents::THIS && !self.has_this() {
                    if self.env.context.active_enum_class.is_some() {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::enum_class_references_this,
                        ))
                    }
                    // If we are in the special top level debugger function, lets not check for $this since
                    // it will be properly lifted in closure convert
                    if self
                        .first_parent_function_name()
                        .is_none_or(|s| s == "include")
                    {
                        return;
                    }
                    self.errors
                        .push(make_error_from_node(node, errors::invalid_this))
                }
            }
            PipeVariableExpression(_) => match self.env.context.pipe_variable_state {
                PipeVariableState::Unavailable => self
                    .errors
                    .push(make_error_from_node(node, errors::invalid_pipe_variable)),
                PipeVariableState::Captured => {
                    self.check_can_use_feature(node, &FeatureName::CapturePipeVariables)
                }
                PipeVariableState::Available => {}
            },
            _ => {}
        }
    }

    fn function_call_on_xhp_name_errors(&mut self, node: S<'a>) {
        let check = |self_: &mut Self, member_object: S<'a>, name: S<'a>| {
            if let XHPExpression(_) = &member_object.children {
                if self_.env.is_typechecker() {
                    self_.errors.push(make_error_from_node(
                        node,
                        errors::method_calls_on_xhp_expression,
                    ))
                }
            }

            if let Token(token) = &name.children {
                if token.kind() == TokenKind::XHPClassName {
                    self_.errors.push(make_error_from_node(
                        node,
                        errors::method_calls_on_xhp_attributes,
                    ))
                }
            }
        };
        match &node.children {
            MemberSelectionExpression(x) => check(self, &x.object, &x.name),
            SafeMemberSelectionExpression(x) => check(self, &x.object, &x.name),
            _ => {}
        }
    }

    fn no_async_before_lambda_body(&mut self, body_node: S<'a>) {
        if let AwaitableCreationExpression(_) = &body_node.children {
            if self.env.is_typechecker() {
                self.errors.push(make_error_from_node(
                    body_node,
                    errors::no_async_before_lambda_body,
                ))
            }
        }
    }

    fn no_memoize_attribute_on_lambda(&mut self, node: S<'a>) {
        match &node.children {
            OldAttributeSpecification(_) => {
                for node in attr_spec_to_node_list(node) {
                    match self.attr_name(node) {
                        Some(n) if sn::user_attributes::is_memoized(n) => self
                            .errors
                            .push(make_error_from_node(node, errors::memoize_on_lambda)),

                        _ => {}
                    }
                }
            }

            _ => {}
        }
    }

    fn new_variable_errors_(&mut self, node: S<'a>, inside_scope_resolution: bool) {
        match &node.children {
            SimpleTypeSpecifier(_)
            | VariableExpression(_)
            | GenericTypeSpecifier(_)
            | PipeVariableExpression(_) => {}
            SubscriptExpression(x) if x.index.is_missing() => self.errors.push(
                make_error_from_node(node, errors::instanceof_missing_subscript_index),
            ),
            SubscriptExpression(x) => {
                self.new_variable_errors_(&x.receiver, inside_scope_resolution)
            }
            MemberSelectionExpression(x) => {
                if inside_scope_resolution {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::instanceof_memberselection_inside_scoperesolution,
                    ))
                } else {
                    self.new_variable_errors_(&x.object, inside_scope_resolution)
                }
            }
            ScopeResolutionExpression(x) => {
                if let Token(name) = &x.name.children {
                    if is_good_scope_resolution_qualifier(
                        &x.qualifier,
                        /* allow static */ true,
                    ) && name.kind() == TokenKind::Variable
                    {
                        // OK
                    } else if name.kind() == TokenKind::Variable {
                        self.new_variable_errors_(&x.qualifier, true)
                    } else {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::instanceof_invalid_scope_resolution,
                        ))
                    }
                } else {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::instanceof_invalid_scope_resolution,
                    ))
                }
            }
            _ => {
                self.errors.push(make_error_from_node(
                    node,
                    errors::new_unknown_node(node.kind().to_string()),
                ));
            }
        }
    }

    fn new_variable_errors(&mut self, node: S<'a>) {
        self.new_variable_errors_(node, false)
    }

    fn class_type_designator_errors(&mut self, node: S<'a>) {
        if !is_good_scope_resolution_qualifier(node, /* allow static */ true) {
            match &node.children {
                ParenthesizedExpression(_) => {}
                _ => self.new_variable_errors(node),
            }
        }
    }

    fn rec_walk_impl<F, X>(parents: &mut Vec<S<'a>>, f: &F, node: S<'a>, mut acc: X) -> X
    where
        F: Fn(S<'a>, &Vec<S<'a>>, X) -> (bool, X),
    {
        let (continue_walk, new_acc) = f(node, parents, acc);
        acc = new_acc;
        if continue_walk {
            parents.push(node);
            for child in node.iter_children() {
                acc = Self::rec_walk_impl(parents, f, child, acc);
            }
            parents.pop();
        }
        acc
    }

    fn rec_walk<F, X>(&self, f: F, node: S<'a>, acc: X) -> X
    where
        F: Fn(S<'a>, &Vec<S<'a>>, X) -> (bool, X),
    {
        Self::rec_walk_impl(&mut vec![], &f, node, acc)
    }

    fn find_invalid_lval_usage(&self, node: S<'a>) -> Vec<SyntaxError> {
        self.rec_walk(
            |node, parents, mut acc| match &node.children {
                AnonymousFunction(_) | LambdaExpression(_) | AwaitableCreationExpression(_) => {
                    (false, acc)
                }
                _ => {
                    match node_lval_type(node, parents) {
                        LvalTypeFinal | LvalTypeNone => {}
                        LvalTypeNonFinal => {
                            acc.push(make_error_from_node(node, errors::lval_as_expression))
                        }
                        LvalTypeNonFinalInout => {
                            acc.push(make_error_from_node(node, errors::inout_argument_in_await))
                        }
                    };
                    (true, acc)
                }
            },
            node,
            vec![],
        )
    }

    fn await_as_an_expression_errors(&mut self, await_node: S<'a>) {
        let mut prev = None;
        let mut node = await_node;

        // true if we are in an expression tree, false if we are in a splice or not in an expression tree at all
        // Start at false because actual await expressions are forbidden in expression trees
        let mut in_expr_tree = false;
        for n in self.parents.iter().rev() {
            if let Some(prev) = prev {
                node = prev;
            }
            prev = Some(n);
            match &n.children {
                ETSpliceExpression(_) => {
                    let mut errors = std::mem::take(&mut self.errors);
                    self.check_can_use_feature_errors(
                        &mut errors,
                        node,
                        &FeatureName::AwaitInSplice,
                    );
                    self.errors = errors;
                    // if we were in a splice, the parent must be in an expression tree, because splices can only occur in expression trees
                    in_expr_tree = true;
                    continue;
                }
                PrefixedCodeExpression(_) => {
                    // if we were in an expression tree, the parent must not be, since expression trees can't directly nest
                    in_expr_tree = false;
                    continue;
                }
                _ if in_expr_tree => {
                    // if we are in an expression tree, than nothing affects the execution of an await nested in a splice
                    // so we ignore, even if it's a lambda, expressionStatement, etc. that would usually break us out of
                    // the check
                    continue;
                }
                // statements that root for the concurrently executed await expressions, as long as they aren't in an expression tree
                ExpressionStatement(_)
                | ReturnStatement(_)
                | UnsetStatement(_)
                | EchoStatement(_)
                | ThrowStatement(_) => break,
                IfStatement(x) if std::ptr::eq(node, &x.condition) => break,
                ForStatement(x) if std::ptr::eq(node, &x.initializer) => break,
                SwitchStatement(x) if std::ptr::eq(node, &x.expression) => break,
                ForeachStatement(x) if std::ptr::eq(node, &x.collection) => {
                    break;
                }
                UsingStatementBlockScoped(x) if std::ptr::eq(node, &x.expressions) => {
                    break;
                }
                UsingStatementFunctionScoped(x) if std::ptr::eq(node, &x.expression) => {
                    break;
                }
                LambdaExpression(x) if std::ptr::eq(node, &x.body) => break,
                PrefixUnaryExpression(x) if token_kind(&x.operator) == Some(TokenKind::Await) => {
                    let (feature, enabled) = self.is_pipe_await_enabled();
                    if !enabled {
                        self.errors.push(make_error_from_node(
                            await_node,
                            Cow::Owned(format!(
                            "`await` cannot be used as an expression inside another await expression, unless unstable feature: `{}` is enabled. Pull the inner `await` out into its own statement.",
                            feature)
                        )));
                        break;
                    }
                    continue;
                }
                // Unary based expressions have their own custom fanout
                PrefixUnaryExpression(x) if unop_allows_await(&x.operator) => {
                    continue;
                }
                PostfixUnaryExpression(x) if unop_allows_await(&x.operator) => {
                    continue;
                }
                DecoratedExpression(x) if unop_allows_await(&x.decorator) => {
                    continue;
                }
                // Special case the pipe operator error message
                BinaryExpression(x)
                    if std::ptr::eq(node, &x.right_operand)
                        && token_kind(&x.operator) == Some(TokenKind::BarGreaterThan) =>
                {
                    let (feature, enabled) = self.is_pipe_await_enabled();
                    if !enabled {
                        self.errors.push(make_error_from_node(
                            node,
                            Cow::Owned(format!(
                                "`await` cannot be used as an expression right of a pipe operator, unless unstable feature: `{}` is enabled.",
                                feature
                            )),
                        ))
                    }
                    continue;
                }
                // left or right operand of binary expressions are considered legal locations
                // if operator is not short-circuiting and containing expression
                // is in legal location
                BinaryExpression(x)
                    if (match get_positions_binop_allows_await(&x.operator) {
                        BinopAllowAwaitBoth => true,
                        BinopAllowAwaitLeft => std::ptr::eq(node, &x.left_operand),
                        BinopAllowAwaitRight => std::ptr::eq(node, &x.right_operand),
                        BinopAllowAwaitNone => false,
                    }) =>
                {
                    continue;
                }
                // test part of conditional expression is considered legal location if
                //  onditional expression itself is in legal location
                ConditionalExpression(x) if std::ptr::eq(node, &x.test) => {
                    continue;
                }
                FunctionCallExpression(x)
                    if std::ptr::eq(node, &x.receiver)
                        || std::ptr::eq(node, &x.argument_list)
                            && !x.receiver.is_safe_member_selection_expression() =>
                {
                    continue;
                }

                // object of member selection expression or safe member selection expression
                // is in legal position if member selection expression itself is in legal position
                SafeMemberSelectionExpression(x) if std::ptr::eq(node, &x.object) => {
                    continue;
                }

                ParameterDeclaration(_) => {
                    self.errors.push(make_error_from_node(
                            await_node,
                            errors::invalid_await_param_default,
                    ));
                    break;
                }

                // These are nodes where any position is valid
                CastExpression(_)
                | MemberSelectionExpression(_)
                | ScopeResolutionExpression(_)
                | IsExpression(_)
                | AsExpression(_)
                | NullableAsExpression(_)
                | IssetExpression(_)
                | ParenthesizedExpression(_)
                | BracedExpression(_)
                | EmbeddedBracedExpression(_)
                | CollectionLiteralExpression(_)
                | ObjectCreationExpression(_)
                | ConstructorCall(_)
                | ShapeExpression(_)
                | TupleExpression(_)
                | DarrayIntrinsicExpression(_)
                | DictionaryIntrinsicExpression(_)
                | KeysetIntrinsicExpression(_)
                | VarrayIntrinsicExpression(_)
                | VectorIntrinsicExpression(_)
                | ElementInitializer(_)
                | FieldInitializer(_)
                | SimpleInitializer(_)
                | SubscriptExpression(_)
                | EmbeddedSubscriptExpression(_)
                | EmbeddedMemberSelectionExpression(_)
                | YieldExpression(_)
                | XHPExpression(_)
                | XHPOpen(_)
                | XHPSimpleAttribute(_)
                | XHPSpreadAttribute(_)
                | SyntaxList(_)
                | ListItem(_) => continue,

                // Unconditional await locations that were erroneously banned
                // as conditional
                PrefixUnaryExpression(_) // inc, dec
                | PostfixUnaryExpression(_) // inc, dec
                | DecoratedExpression(_) // ... and inout
                | LiteralExpression(_) // "{$foo(await bar())}"
                | PrefixedStringExpression(_) // r"{$foo(await bar())}"
                | MatchStatement(_) // match (await $x)
                | EvalExpression(_) // eval(await $x)
                | ListExpression(_) // list($a[await $b], ...) = ...
                | InclusionExpression(_) => { // include await $x
                    if !FeatureName::AllowConditionalAwaitSyntax.can_use(
                        &self.env.parser_options,
                        &self.env.context.active_experimental_features,
                    ) {
                        self.check_can_use_feature(node, &FeatureName::AllowConditionalAwaitSyntax);
                        break;
                    }
                    continue;
                }

                // Statements with conditional subexpressions that permit
                // awaits
                WhileStatement(_) // while (await $x)
                | DoStatement(_) // do { ... } while (await $x)
                | ForStatement(_) => { // for (; await x(); $y += await z())
                    self.check_can_use_feature(node, &FeatureName::AllowConditionalAwaitSyntax);
                    break;
                }

                // Conditional expressions that can contain awaits
                BinaryExpression(_) // ||, &&, and ??
                | ConditionalExpression(_) // a ? b : c, a ?: b
                | FunctionCallExpression(_) // a?->b()
                | SafeMemberSelectionExpression(_) => { // a?->b
                    if !FeatureName::AllowConditionalAwaitSyntax.can_use(
                        &self.env.parser_options,
                        &self.env.context.active_experimental_features,
                    ) {
                        self.check_can_use_feature(node, &FeatureName::AllowConditionalAwaitSyntax);
                        break;
                    }
                    continue;
                }

                // otherwise report error and bail out
                _ => {
                    self.errors.push(make_error_from_node(
                            await_node,
                            errors::invalid_await_position,
                    ));
                    break;
                }
            }
        }
        let is_in_concurrent = self
            .parents
            .iter()
            .rev()
            .any(|parent| match &parent.children {
                ConcurrentStatement(_) => true,
                _ => false,
            });
        if !is_in_concurrent {
            let await_node_statement_parent =
                self.parents
                    .iter()
                    .rev()
                    .find(|parent| match &parent.children {
                        ExpressionStatement(_)
                        | ReturnStatement(_)
                        | UnsetStatement(_)
                        | EchoStatement(_)
                        | ThrowStatement(_)
                        | IfStatement(_)
                        | ForStatement(_)
                        | SwitchStatement(_)
                        | ForeachStatement(_) => true,
                        _ => false,
                    });
            if let Some(x) = await_node_statement_parent {
                for error in self.find_invalid_lval_usage(x) {
                    self.errors.push(error)
                }
            } else {
                // We must have already errored in for loop
            }
        }
    }

    fn is_pipe_await_enabled(&self) -> (FeatureName, bool) {
        let feature = FeatureName::PipeAwait;
        let enabled = feature.can_use(
            &self.env.parser_options,
            &self.env.context.active_experimental_features,
        );
        (feature, enabled)
    }

    fn node_has_await_child(&mut self, node: S<'a>) -> bool {
        self.rec_walk(
            |node, _parents, acc| {
                let is_new_scope = match &node.children {
                    AnonymousFunction(_) | LambdaExpression(_) | AwaitableCreationExpression(_) => {
                        true
                    }
                    _ => false,
                };
                if is_new_scope {
                    (false, false)
                } else {
                    let is_await = |n: S<'a>| match &n.children {
                        PrefixUnaryExpression(x)
                            if token_kind(&x.operator) == Some(TokenKind::Await) =>
                        {
                            true
                        }
                        _ => false,
                    };
                    let found_await = acc || is_await(node);
                    (!found_await, found_await)
                }
            },
            node,
            false,
        )
    }

    fn expression_errors(&mut self, node: S<'a>) {
        let check_is_as_expression = |self_: &mut Self, hint: S<'a>| {
            let n = match &node.children {
                IsExpression(_) => "is",
                _ => "as",
            };
            match &hint.children {
                ClosureTypeSpecifier(_) if self_.env.is_hhvm_compat() => {
                    self_.errors.push(make_error_from_node(
                        hint,
                        errors::invalid_is_as_expression_hint(n, "__Callable"),
                    ));
                }
                AttributizedSpecifier(x)
                    if self_.attribute_specification_contains(&x.attribute_spec, "__Soft") =>
                {
                    self_.errors.push(make_error_from_node(
                        hint,
                        errors::invalid_is_as_expression_hint(n, "__Soft"),
                    ));
                }
                _ => {}
            }
        };
        match &node.children {
            // We parse the right hand side of `new` as a generic expression, but PHP
            // (and therefore Hack) only allow a certain subset of expressions, so we
            // should verify here that the expression we parsed is in that subset.
            // Refer: https://github.com/php/php-langspec/blob/master/spec/10-expressions.md#instanceof-operator*)
            ConstructorCall(ctr_call) => {
                for p in syntax_to_list_no_separators(&ctr_call.argument_list) {
                    if let DecoratedExpression(e) = &p.children {
                        if let Token(t) = &e.decorator.children {
                            if t.kind() == TokenKind::Inout {
                                self.errors.push(make_error_from_node(
                                    p,
                                    errors::inout_param_in_construct,
                                ));
                            }
                        }
                    }
                }
                self.class_type_designator_errors(&ctr_call.type_);
                if self.env.is_typechecker() {
                    // attr or list item -> syntax list -> attribute
                    match self.parents.iter().rev().nth(2) {
                        Some(a)
                            if a.is_old_attribute_specification()
                                || a.is_file_attribute_specification() => {}
                        _ => {
                            if ctr_call.left_paren.is_missing() || ctr_call.right_paren.is_missing()
                            {
                                let node = &ctr_call.type_;
                                let constructor_name = self.text(&ctr_call.type_);
                                self.errors.push(make_error_from_node(
                                    node,
                                    errors::error2038(constructor_name),
                                ));
                            }
                        }
                    }
                };
            }
            LiteralExpression(x) => {
                if let Token(token) = &x.expression.children {
                    if token.kind() == TokenKind::DecimalLiteral
                        || token.kind() == TokenKind::DecimalLiteral
                    {
                        let text = self.text(&x.expression);
                        if text.parse::<i64>().is_err() {
                            let error_text = if token.kind() == TokenKind::DecimalLiteral {
                                errors::error2071(text)
                            } else {
                                errors::error2072(text)
                            };
                            self.errors.push(make_error_from_node(node, error_text))
                        }
                    }
                }
            }

            FunctionCallExpression(x) => {
                let arg_list = &x.argument_list;
                if let Some(h) = misplaced_splat_arg(arg_list) {
                    self.errors.push(make_error_from_node(h, errors::error2033))
                }

                let recv = &x.receiver;

                self.function_call_on_xhp_name_errors(recv);

                if strip_ns(self.text(recv)) == strip_ns(sn::readonly::AS_MUT) {
                    self.mark_uses_readonly()
                }
            }

            ETSpliceExpression(_) => {
                if !self.context.active_expression_tree && self.context.expression_tree_depth == 0 {
                    // If the depth is greater than 0, then the error in et_splice_expr will be used
                    self.errors
                        .push(make_error_from_node(node, errors::splice_outside_et))
                }
            }

            ListExpression(x) if x.members.is_missing() && self.env.is_hhvm_compat() => {
                if let Some(Syntax {
                    children: ForeachStatement(x),
                    ..
                }) = self.parents.last()
                {
                    if std::ptr::eq(node, &x.value) {
                        self.errors.push(make_error_from_node_with_type(
                            node,
                            errors::error2077,
                            ErrorType::RuntimeError,
                        ))
                    }
                }
            }

            ListExpression(_) => {
                if self.parents.last().is_some_and(|e| e.is_return_statement()) {
                    self.errors
                        .push(make_error_from_node(node, errors::list_must_be_lvar))
                }
            }
            ShapeExpression(x) => {
                if self.context.active_expression_tree {
                    self.check_can_use_feature(node, &FeatureName::ExpressionTreeShapeCreation)
                }
                let is_destructure_lhs = {
                    let mut found = false;
                    let mut child: S<'a> = node;
                    for &parent in self.parents.iter().rev() {
                        match &parent.children {
                            FieldInitializer(_)
                            | ShapeExpression(_)
                            | TupleExpression(_)
                            | ListItem(_)
                            | SyntaxList(_)
                            | PrefixUnaryExpression(_) => {
                                child = parent;
                                continue;
                            }
                            BinaryExpression(bx) => {
                                found = token_kind(&bx.operator) == Some(TokenKind::Equal)
                                    && std::ptr::eq(child, &bx.left_operand);
                                break;
                            }
                            ForeachStatement(fx) => {
                                found = std::ptr::eq(child, &fx.value);
                                break;
                            }
                            _ => break,
                        }
                    }
                    found
                };
                for f in syntax_to_list_no_separators(&x.fields).rev() {
                    self.invalid_shape_field_check(f, is_destructure_lhs)
                }
            }
            VectorIntrinsicExpression(_)
            | DictionaryIntrinsicExpression(_)
            | KeysetIntrinsicExpression(_) => {
                if self.context.active_expression_tree {
                    self.check_can_use_feature(node, &FeatureName::ExpressionTreeHackArrays)
                }
            }
            DecoratedExpression(x) => {
                let decorator = &x.decorator;
                if token_kind(decorator) == Some(TokenKind::Await) {
                    self.await_as_an_expression_errors(node)
                }
            }
            YieldExpression(_) => {
                if self.is_in_unyieldable_magic_method() {
                    self.errors
                        .push(make_error_from_node(node, errors::yield_in_magic_methods))
                }
                if self.env.context.active_callable.is_none() {
                    self.errors
                        .push(make_error_from_node(node, errors::yield_outside_function))
                }

                if self.has_inout_params() {
                    let e = if self.is_inside_async_method() {
                        errors::inout_param_in_async_generator
                    } else {
                        errors::inout_param_in_generator
                    };
                    self.errors.push(make_error_from_node_with_type(
                        node,
                        e,
                        ErrorType::RuntimeError,
                    ))
                }
            }
            ScopeResolutionExpression(x) => {
                let qualifier = &x.qualifier;
                let name = &x.name;

                let (is_dynamic_name, is_self_or_parent, is_valid) =
                    // PHP langspec allows string literals, variables
                    // qualified names, static, self and parent as valid qualifiers
                    // We do not allow string literals in hack
                    match (&qualifier.children, token_kind(qualifier)) {
                            (LiteralExpression(_), _) => (false, false, false),
                            (QualifiedName(_), _) => (false, false, true),
                            (_, Some(TokenKind::Name))
                            | (_, Some(TokenKind::XHPClassName))
                            | (_, Some(TokenKind::Static)) => (false, false, true),
                            (_, Some(TokenKind::SelfToken)) | (_, Some(TokenKind::Parent)) => {
                                (false, true, true)
                            }
                            // ${}::class
                            (PrefixUnaryExpression(x), _)
                                if token_kind(&x.operator) == Some(TokenKind::Dollar) =>
                            {
                                (true, false, true)
                            }
                            (PipeVariableExpression(_), _)
                            | (VariableExpression(_), _)
                            | (SimpleTypeSpecifier(_), _)
                            | (GenericTypeSpecifier(_), _) => (true, false, true),
                            _ => (true, false, false),
                        };
                if !is_valid {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_scope_resolution_qualifier,
                    ))
                }
                let is_name_class = self.text(name).eq_ignore_ascii_case("class");
                if (is_dynamic_name || !is_valid) && is_name_class {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::coloncolonclass_on_dynamic,
                    ))
                }
                let text_name = self.text(qualifier);
                let is_name_namespace = text_name.eq_ignore_ascii_case("namespace");
                if is_name_namespace {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::namespace_not_a_classname,
                    ))
                }
                if is_self_or_parent && is_name_class && !self.is_in_active_class_scope() {
                    self.errors.push(make_error_from_node_with_type(
                        node,
                        errors::self_or_parent_colon_colon_class_outside_of_class(text_name),
                        ErrorType::RuntimeError,
                    ))
                }
            }

            PrefixUnaryExpression(x) if token_kind(&x.operator) == Some(TokenKind::Dollar) => {
                if check_prefix_unary_dollar(node) {
                    self.errors
                        .push(make_error_from_node(node, errors::dollar_unary))
                }
            }

            IsExpression(x) => check_is_as_expression(self, &x.right_operand),
            AsExpression(x) => check_is_as_expression(self, &x.right_operand),

            ConditionalExpression(x) => {
                if x.consequence.is_missing() && self.env.is_typechecker() {
                    self.errors
                        .push(make_error_from_node(node, errors::elvis_operator_space))
                }
                if x.test.is_conditional_expression() && self.env.is_typechecker() {
                    self.errors
                        .push(make_error_from_node(node, errors::nested_ternary))
                }
                match &x.alternative.children {
                    LambdaExpression(x)
                        if x.body.is_conditional_expression() && self.env.is_typechecker() =>
                    {
                        self.errors
                            .push(make_error_from_node(node, errors::nested_ternary))
                    }
                    _ => {}
                }
            }
            LambdaExpression(x) => {
                self.no_memoize_attribute_on_lambda(&x.attribute_spec);
                self.no_async_before_lambda_body(&x.body);
            }
            AnonymousFunction(x) => self.no_memoize_attribute_on_lambda(&x.attribute_spec),
            AwaitableCreationExpression(x) => {
                self.no_memoize_attribute_on_lambda(&x.attribute_spec)
            }

            CollectionLiteralExpression(x) => {
                #[allow(dead_code)] // field `0` is never read
                enum Status {
                    ValidClass(String),
                    ValidClassEt(String),
                    InvalidClass,
                    InvalidBraceKind,
                }
                use Status::*;

                let n = &x.name;
                let initializers = &x.initializers;

                let is_standard_collection = |lc_name: &str| {
                    lc_name.eq_ignore_ascii_case("pair")
                        || lc_name.eq_ignore_ascii_case("vector")
                        || lc_name.eq_ignore_ascii_case("map")
                        || lc_name.eq_ignore_ascii_case("set")
                        || lc_name.eq_ignore_ascii_case("immvector")
                        || lc_name.eq_ignore_ascii_case("immmap")
                        || lc_name.eq_ignore_ascii_case("immset")
                };
                let use_key_value_initializers = |lc_name: &str| {
                    lc_name.eq_ignore_ascii_case("map") || lc_name.eq_ignore_ascii_case("immmap")
                };
                let use_vector_initializers = |lc_name: &str| {
                    lc_name.eq_ignore_ascii_case("pair")
                        || lc_name.eq_ignore_ascii_case("vector")
                        || lc_name.eq_ignore_ascii_case("set")
                        || lc_name.eq_ignore_ascii_case("immvector")
                        || lc_name.eq_ignore_ascii_case("immset")
                };
                let is_qualified_std_collection = |l, r| {
                    token_kind(l) == Some(TokenKind::Name)
                        && token_kind(r) == Some(TokenKind::Name)
                        && self.text(l).eq_ignore_ascii_case("hh")
                        && is_standard_collection(self.text(r))
                };

                let check_type_specifier = |n, t: &PositionedToken<'a>| {
                    if t.kind() == TokenKind::Name {
                        match self.text(n).to_ascii_lowercase().as_ref() {
                            "dict" | "vec" | "keyset" => InvalidBraceKind,
                            n => {
                                if self.context.active_expression_tree {
                                    ValidClassEt(n.to_string())
                                } else if is_standard_collection(n) {
                                    ValidClass(n.to_string())
                                } else {
                                    InvalidClass
                                }
                            }
                        }
                    } else {
                        InvalidClass
                    }
                };

                let check_qualified_name = |parts| {
                    let mut parts = syntax_to_list(false, parts);
                    let p1 = parts.next();
                    let p2 = parts.next();
                    let p3 = parts.next();
                    let p4 = parts.next();
                    match (p1, p2, p3, p4) {
                        (Some(l), Some(r), None, None)
                            if self.namespace_name == GLOBAL_NAMESPACE_NAME
                                && is_qualified_std_collection(l, r) =>
                        {
                            // HH\Vector in global namespace
                            ValidClass(self.text(r).to_ascii_lowercase())
                        }
                        (Some(missing), Some(l), Some(r), None)
                            if missing.is_missing() && is_qualified_std_collection(l, r) =>
                        {
                            // \HH\Vector
                            ValidClass(self.text(r).to_ascii_lowercase())
                        }
                        _ => InvalidClass,
                    }
                };
                let status = match &n.children {
                    // non-qualified name
                    SimpleTypeSpecifier(x) => match &x.specifier.children {
                        Token(t) => check_type_specifier(&x.specifier, t),
                        QualifiedName(x) => check_qualified_name(&x.parts),
                        _ => InvalidClass,
                    },
                    GenericTypeSpecifier(x) => match &x.class_type.children {
                        Token(t) => check_type_specifier(&x.class_type, t),
                        QualifiedName(x) => check_qualified_name(&x.parts),
                        _ => InvalidClass,
                    },
                    _ => InvalidClass,
                };

                let is_key_value = |s: S<'a>| {
                    if let ElementInitializer(_) = s.children {
                        true
                    } else {
                        false
                    }
                };
                let initializer_list = || syntax_to_list_no_separators(initializers);
                let num_initializers = initializer_list().count();
                match &status {
                    ValidClass(name)
                        if use_vector_initializers(name)
                            && initializer_list().any(is_key_value) =>
                    {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::invalid_key_value_initializer(self.text(n)),
                        ));
                    }

                    ValidClass(name)
                        if (use_key_value_initializers(name)
                            || self.context.active_expression_tree)
                            && initializer_list().any(|i| !is_key_value(i)) =>
                    {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::invalid_value_initializer(self.text(n)),
                        ));
                    }

                    ValidClass(pair) if pair == "pair" && num_initializers != 2 => {
                        let msg = if num_initializers == 0 {
                            errors::pair_initializer_needed
                        } else {
                            errors::pair_initializer_arity
                        };
                        self.errors.push(make_error_from_node_with_type(
                            node,
                            msg,
                            ErrorType::RuntimeError,
                        ));
                    }

                    ValidClass(_) => {}
                    ValidClassEt(_) => {}
                    InvalidBraceKind => self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_brace_kind_in_collection_initializer,
                    )),
                    InvalidClass => self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_class_in_collection_initializer,
                    )),
                }
            }
            PrefixUnaryExpression(x) if token_kind(&x.operator) == Some(TokenKind::Await) => {
                self.await_as_an_expression_errors(node)
            }
            PrefixUnaryExpression(x) if token_kind(&x.operator) == Some(TokenKind::Readonly) => {
                self.mark_uses_readonly()
            }
            DarrayIntrinsicExpression(_) | VarrayIntrinsicExpression(_)
                if self.env.is_typechecker() =>
            {
                self.errors
                    .push(make_error_from_node(node, errors::varray_darray_banned))
            }
            SafeMemberSelectionExpression(_) if self.context.active_expression_tree => {
                self.check_can_use_feature(node, &FeatureName::ExpressionTreeNullsafeObjGet);
            }

            // Other kinds of expressions currently produce no expr errors.
            _ => {}
        }
    }

    fn check_repeated_properties_tconst_const(
        &mut self,
        full_name: &str,
        prop: S<'a>,
        p_names: &mut HashSet<String>,
        c_names: &mut HashSet<String>,
        xhp_names: &mut HashSet<String>,
    ) {
        let mut check = |sname, names: &mut HashSet<String>| {
            let name = self.text(sname);
            // If the name is empty, then there was an earlier
            // parsing error that should supercede this one.
            if name.is_empty() {
            } else if names.contains(name) {
                self.errors.push(make_error_from_node(
                    prop,
                    errors::redeclaration_error(&(strip_ns(full_name).to_string() + "::" + name)),
                ))
            } else {
                names.insert(name.to_owned());
            }
        };

        match &prop.children {
            PropertyDeclaration(x) => {
                for prop in syntax_to_list_no_separators(&x.declarators) {
                    if let PropertyDeclarator(x) = &prop.children {
                        check(&x.name, p_names)
                    }
                }
            }
            ConstDeclaration(x) => {
                for prop in syntax_to_list_no_separators(&x.declarators) {
                    if let ConstantDeclarator(x) = &prop.children {
                        check(&x.name, c_names)
                    }
                }
            }
            TypeConstDeclaration(x) => check(&x.name, c_names),
            ContextConstDeclaration(x) => check(&x.name, c_names),
            XHPClassAttributeDeclaration(x) => {
                for attr in syntax_to_list_no_separators(&x.attributes) {
                    if let XHPClassAttribute(x) = &attr.children {
                        check(&x.name, xhp_names)
                    }
                }
            }
            _ => {}
        }
    }

    fn require_errors(&mut self, node: S<'a>) {
        if let RequireClause(p) = &node.children {
            let name = self.text(&p.name);
            let req_kind = token_kind(&p.kind);
            match (self.trait_require_clauses.get(name), req_kind) {
                (None, Some(tk)) => self.trait_require_clauses.add(name, tk),
                (Some(tk1), Some(tk2)) if *tk1 == tk2 =>
                    // duplicate, it is okay
                    {}
                _ => {
                    // Conflicting entry
                    self.errors.push(make_error_from_node(
                        node,
                        errors::conflicting_trait_require_clauses(name),
                    ))
                }
            };
            match (self.active_classish_kind(), req_kind) {
                (Some(TokenKind::Interface), Some(TokenKind::Implements))
                | (Some(TokenKind::Class), Some(TokenKind::Implements)) => self
                    .errors
                    .push(make_error_from_node(node, errors::error2030)),
                _ => {}
            }
        }
    }

    fn check_alias_name(&mut self, name: S<'a>, name_text: &str, location: Location) {
        self.produce_error(
            |_, x| cant_be_reserved_type_name(x),
            name_text,
            || errors::reserved_keyword_as_type_name(name_text),
            name,
        );
        self.check_use_type_name(name, name_text, location);
    }

    /// This reports "name already in use" relating to namespace use statements.
    fn check_use_type_name(&mut self, name: S<'a>, name_text: &str, location: Location) {
        match self.names.classes.get(name_text) {
            Some(FirstUseOrDef {
                location,
                kind,
                name: def_name,
                ..
            }) if &combine_names(&self.namespace_name, name_text) != def_name
                && *kind != NameDef =>
            {
                let (line_num, _) = self
                    .env
                    .text
                    .offset_to_position(location.start_offset as isize);
                let line_num = line_num as usize;
                let long_name_text = combine_names(&self.namespace_name, name_text);
                self.errors.push(make_name_already_used_error(
                    name,
                    &long_name_text,
                    name_text,
                    location,
                    &|x, y| match kind {
                        NameImplicitUse => {
                            errors::declared_name_is_already_in_use_implicit_hh(line_num, x, y)
                        }
                        NameUse => errors::declared_name_is_already_in_use(line_num, x, y),
                        NameDef => errors::type_name_is_already_in_use(x, y),
                    },
                ))
            }
            _ => {
                let def = make_first_use_or_def(
                    false,
                    NameDef,
                    location,
                    &self.namespace_name,
                    name_text,
                );
                self.names.classes.add(name_text, def)
            }
        }
    }

    fn get_type_params_and_emit_shadowing_errors(
        &mut self,
        l: S<'a>,
    ) -> (HashSet<&'a str>, HashSet<&'a str>) {
        let mut res: HashSet<&'a str> = HashSet::default();
        let mut notreified: HashSet<&'a str> = HashSet::default();
        for p in syntax_to_list_no_separators(l).rev() {
            match &p.children {
                TypeParameter(x) => {
                    let name = self.text(&x.name);
                    if !x.reified.is_missing() {
                        if res.contains(&name) {
                            self.errors
                                .push(make_error_from_node(p, errors::shadowing_reified))
                        } else {
                            res.insert(name);
                        }
                    } else {
                        notreified.insert(name);
                    }
                }
                _ => {}
            }
        }
        (res, notreified)
    }

    fn class_reified_param_errors(&mut self, node: S<'a>) {
        match &node.children {
            ClassishDeclaration(cd) => {
                let (reified, non_reified) = match &cd.type_parameters.children {
                    TypeParameters(x) => {
                        self.get_type_params_and_emit_shadowing_errors(&x.parameters)
                    }
                    _ => (HashSet::default(), HashSet::default()),
                };

                let tparams: HashSet<&'a str> = reified
                    .union(&non_reified)
                    .cloned()
                    .collect::<HashSet<&'a str>>();

                let add_error = |self_: &mut Self, e: S<'a>| {
                    if let TypeParameter(x) = &e.children {
                        if !x.reified.is_missing() && tparams.contains(&self_.text(&x.name)) {
                            self_
                                .errors
                                .push(make_error_from_node(e, errors::shadowing_reified))
                        }
                    }
                };
                let check_method = |e: S<'a>| {
                    if let MethodishDeclaration(x) = &e.children {
                        if let FunctionDeclarationHeader(x) = &x.function_decl_header.children {
                            if let TypeParameters(x) = &x.type_parameter_list.children {
                                syntax_to_list_no_separators(&x.parameters)
                                    .rev()
                                    .for_each(|x| add_error(self, x))
                            }
                        }
                    }
                };
                if let ClassishBody(x) = &cd.body.children {
                    syntax_to_list_no_separators(&x.elements)
                        .rev()
                        .for_each(check_method)
                }

                if !reified.is_empty() {
                    if is_token_kind(&cd.keyword, TokenKind::Interface) {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::reified_in_invalid_classish("an interface"),
                        ))
                    } else if is_token_kind(&cd.keyword, TokenKind::Trait) {
                        self.errors.push(make_error_from_node(
                            node,
                            errors::reified_in_invalid_classish("a trait"),
                        ))
                    }
                }
            }
            PropertyDeclaration(_) => {
                if has_modifier_static(node) && self.is_in_reified_class() {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::static_property_in_reified_class,
                    ));
                }
            }
            _ => {}
        }
    }

    fn attr_spec_contains_sealed(&self, node: S<'a>) -> bool {
        self.attribute_specification_contains(node, sn::user_attributes::SEALED)
    }

    fn attr_spec_contains_enum_class(&self, node: S<'a>) -> bool {
        self.attribute_specification_contains(node, sn::user_attributes::ENUM_CLASS)
    }

    fn attr_spec_contains_const(&self, node: S<'a>) -> bool {
        self.attribute_specification_contains(node, sn::user_attributes::CONST)
    }

    // If there's more than one XHP category, report an error on the last one.
    fn duplicate_xhp_category_errors<I>(&mut self, elts: I)
    where
        I: Iterator<Item = S<'a>>,
    {
        let mut iter = elts.filter(|x| matches!(&x.children, XHPCategoryDeclaration(_)));
        iter.next();
        if let Some(node) = iter.last() {
            self.errors.push(make_error_from_node(
                node,
                errors::xhp_class_multiple_category_decls,
            ))
        }
    }

    // If there's more than one XHP children declaration, report an error
    // on the last one.
    fn duplicate_xhp_children_errors<I>(&mut self, elts: I)
    where
        I: Iterator<Item = S<'a>>,
    {
        let mut iter = elts.filter(|x| matches!(&x.children, XHPChildrenDeclaration(_)));
        iter.next();
        if let Some(node) = iter.last() {
            self.errors.push(make_error_from_node(
                node,
                errors::xhp_class_multiple_children_decls,
            ))
        }
    }

    fn interface_private_method_errors<I>(&mut self, elts: I)
    where
        I: Iterator<Item = S<'a>>,
    {
        for elt in elts {
            if let Some(modifiers) = get_modifiers_of_declaration(elt) {
                for modifier in syntax_to_list_no_separators(modifiers) {
                    if modifier.is_private() {
                        self.errors.push(make_error_from_node(
                            modifier,
                            errors::interface_has_private_method,
                        ))
                    }
                }
            }
        }
    }

    fn enum_class_errors(&mut self, node: S<'a>) {
        if let EnumClassDeclaration(c) = &node.children {
            let name = self.text(&c.name);
            self.produce_error(
                |_, x| cant_be_reserved_type_name(x),
                name,
                || errors::reserved_keyword_as_type_name(name),
                &c.name,
            );
            self.invalid_modifier_errors("Enum classes", node, |kind| {
                kind == TokenKind::Abstract
                    || kind == TokenKind::Internal
                    || kind == TokenKind::Public
            });
        }
    }

    fn enum_class_enumerator_errors(&mut self, node: S<'a>) {
        if let EnumClassEnumerator(e) = node.children {
            // only allow abstract as modifier + detect modifier duplication
            self.invalid_modifier_errors("Enum class constants", node, |kind| {
                kind == TokenKind::Abstract
            });

            let is_abstract = has_modifier_abstract(node);
            let has_initializer = !e.initializer.is_missing();
            if is_abstract && has_initializer {
                self.errors.push(make_error_from_node(
                    node,
                    errors::enum_class_abstract_constant_with_value,
                ))
            }
            if !is_abstract && !has_initializer {
                self.errors.push(make_error_from_node(
                    node,
                    errors::enum_class_constant_missing_initializer,
                ))
            }
            // prevent constants to be named `class`
            if self.text(&e.name).eq_ignore_ascii_case("class") {
                self.errors.push(make_error_from_node(
                    node,
                    errors::enum_class_elem_name_is_class,
                ))
            }
        }
    }

    fn classish_errors(&mut self, node: S<'a>) {
        if let ClassishDeclaration(cd) = &node.children {
            // Given a ClassishDeclaration node, test whether or not it's a trait
            // invoking the 'extends' keyword.
            let classish_invalid_extends_keyword = |_| {
                // Invalid if uses 'extends' and is a trait.
                token_kind(&cd.extends_keyword) == Some(TokenKind::Extends)
                    && token_kind(&cd.keyword) == Some(TokenKind::Trait)
            };

            let abstract_keyword = extract_keyword(|x| x.is_abstract(), node).unwrap_or(node);

            self.produce_error(
                |self_, x| self_.is_classish_kind_declared_abstract(x),
                node,
                || errors::error2042,
                abstract_keyword,
            );

            // Given a sealed ClassishDeclaration node, test whether all the params
            // are classnames.
            let classish_sealed_arg_not_classname = |self_: &mut Self| {
                attr_spec_to_node_list(&cd.attribute).any(|node| {
                    self_.attr_name(node) == Some(sn::user_attributes::SEALED)
                        && self_.attr_args(node).is_some_and(|mut args| {
                            args.any(|arg_node| match &arg_node.children {
                                ScopeResolutionExpression(x) => self_.text(&x.name) != "class",
                                _ => true,
                            })
                        })
                })
            };

            // Only "regular" class names are allowed in `__Sealed()`
            // attributes.
            for node in attr_spec_to_node_list(&cd.attribute) {
                if (self.attr_name(node)) == Some(sn::user_attributes::SEALED) {
                    match self.attr_args(node) {
                        Some(args) => {
                            for arg in args {
                                match &arg.children {
                                    ScopeResolutionExpression(x) => {
                                        let txt = self.text(&x.qualifier);
                                        let excludes = [
                                            sn::classes::SELF,
                                            sn::classes::PARENT,
                                            sn::classes::STATIC,
                                        ];
                                        if excludes.contains(&txt) {
                                            self.errors.push(make_error_from_node(
                                                &x.qualifier,
                                                errors::sealed_qualifier_invalid,
                                            ));
                                        }
                                    }
                                    _ => {}
                                }
                            }
                        }
                        None => {}
                    }
                }
            }

            self.check_attrs(&cd.attribute);

            let classish_is_sealed = self.attr_spec_contains_sealed(&cd.attribute);

            // Given a ClassishDeclaration node, test whether or not length of
            // extends_list is appropriate for the classish_keyword. *)
            let classish_invalid_extends_list = |self_: &mut Self| {
                // Invalid if is a class and has list of length greater than one.
                self_.env.is_typechecker()
                    && token_kind(&cd.keyword) == Some(TokenKind::Class)
                    && token_kind(&cd.extends_keyword) == Some(TokenKind::Extends)
                    && syntax_to_list_no_separators(&cd.extends_list).count() != 1
            };

            // Given a ClassishDeclaration node, test whether it is sealed and final.
            let classish_sealed_final =
                |_| list_contains_predicate(|x| x.is_final(), &cd.modifiers) && classish_is_sealed;

            self.produce_error(
                |self_, _| classish_invalid_extends_list(self_),
                &(),
                || errors::error2037,
                &cd.extends_list,
            );
            self.invalid_modifier_errors("Classes, interfaces, and traits", node, |kind| {
                kind == TokenKind::Abstract
                    || kind == TokenKind::Final
                    || kind == TokenKind::XHP
                    || kind == TokenKind::Internal
                    || kind == TokenKind::Public
            });

            self.produce_error(
                |self_, _| classish_sealed_arg_not_classname(self_),
                &(),
                || errors::sealed_val_not_classname,
                &cd.attribute,
            );

            self.produce_error(
                |_, x| classish_invalid_extends_keyword(x),
                &(),
                || errors::error2036,
                &cd.extends_keyword,
            );

            self.produce_error(
                |_, x| classish_sealed_final(x),
                &(),
                || errors::sealed_final,
                &cd.attribute,
            );

            let classish_name = self.text(&cd.name);
            self.produce_error(
                |_, x| cant_be_reserved_type_name(x),
                classish_name,
                || errors::reserved_keyword_as_type_name(classish_name),
                &cd.name,
            );
            if is_token_kind(&cd.keyword, TokenKind::Interface)
                && !cd.implements_keyword.is_missing()
            {
                self.errors
                    .push(make_error_from_node(node, errors::interface_implements))
            };
            if self.attr_spec_contains_const(&cd.attribute) {
                if is_token_kind(&cd.keyword, TokenKind::Class)
                    && list_contains_predicate(|x| x.is_abstract(), &cd.modifiers)
                    && list_contains_predicate(|x| x.is_final(), &cd.modifiers)
                {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::no_const_abstract_final_class,
                    ))
                } else if is_token_kind(&cd.keyword, TokenKind::Class)
                    || is_token_kind(&cd.keyword, TokenKind::Interface)
                    || is_token_kind(&cd.keyword, TokenKind::Trait)
                {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::no_const_interfaces_traits_enums_classes,
                    ))
                }
            }

            if self.attr_spec_contains_const(&cd.attribute)
                && is_token_kind(&cd.keyword, TokenKind::Class)
                && list_contains_predicate(|x| x.is_abstract(), &cd.modifiers)
                && list_contains_predicate(|x| x.is_final(), &cd.modifiers)
            {
                self.errors.push(make_error_from_node(
                    node,
                    errors::no_const_abstract_final_class,
                ))
            }

            if list_contains_predicate(|x| x.is_final(), &cd.modifiers) {
                match token_kind(&cd.keyword) {
                    Some(TokenKind::Interface) => self.errors.push(make_error_from_node(
                        node,
                        errors::declared_final("Interfaces"),
                    )),
                    Some(TokenKind::Trait) => self
                        .errors
                        .push(make_error_from_node(node, errors::declared_final("Traits"))),
                    _ => {}
                }
            }

            if token_kind(&cd.xhp) == Some(TokenKind::XHP) {
                match token_kind(&cd.keyword) {
                    Some(TokenKind::Interface) => self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_xhp_classish("Interfaces"),
                    )),
                    Some(TokenKind::Trait) => self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_xhp_classish("Traits"),
                    )),
                    Some(TokenKind::Enum) => self.errors.push(make_error_from_node(
                        node,
                        errors::invalid_xhp_classish("Enums"),
                    )),
                    _ => {}
                }
            }

            let name = self.text(&cd.name);
            if let ClassishBody(cb) = &cd.body.children {
                let declared_name_str = self.text(&cd.name);
                let full_name = combine_names(&self.namespace_name, declared_name_str);

                let class_body_elts = || syntax_to_list_no_separators(&cb.elements);
                let class_body_methods = || class_body_elts().filter(|x| is_method_declaration(x));

                let mut p_names = HashSet::<String>::default();
                let mut c_names = HashSet::<String>::default();
                let mut xhp_names = HashSet::<String>::default();
                for elt in class_body_elts() {
                    self.check_repeated_properties_tconst_const(
                        &full_name,
                        elt,
                        &mut p_names,
                        &mut c_names,
                        &mut xhp_names,
                    );
                }
                let has_abstract_fn = class_body_methods().any(&has_modifier_abstract);
                if has_abstract_fn
                    && is_token_kind(&cd.keyword, TokenKind::Class)
                    && !list_contains_predicate(|x| x.is_abstract(), &cd.modifiers)
                {
                    self.errors.push(make_error_from_node(
                        &cd.name,
                        errors::class_with_abstract_method(name),
                    ))
                }

                if is_token_kind(&cd.keyword, TokenKind::Interface) {
                    self.interface_private_method_errors(class_body_elts());
                }

                self.duplicate_xhp_category_errors(class_body_elts());
                self.duplicate_xhp_children_errors(class_body_elts());
            }

            match token_kind(&cd.keyword) {
                Some(TokenKind::Class) | Some(TokenKind::Trait) if !cd.name.is_missing() => {
                    let location = make_location_of_node(&cd.name);
                    self.check_use_type_name(&cd.name, name, location)
                }
                _ => {}
            }
        }
    }

    // Checks for modifiers on class constants
    fn class_constant_modifier_errors(&mut self, node: S<'a>) {
        self.invalid_modifier_errors("Constants", node, |kind| kind == TokenKind::Abstract);
    }

    fn type_const_modifier_errors(&mut self, node: S<'a>) {
        self.invalid_modifier_errors("Type constants", node, |kind| kind == TokenKind::Abstract);
    }

    fn type_const_bounds_errors(&mut self, node: S<'a>) {
        if self.env.is_typechecker() {
            // HackC & HHVM don't see bounds, so it's pointless to ban (then unban later)
            if let TypeConstDeclaration(tc) = node.children {
                let (super_count, as_count) = tc.type_constraints.iter_children().fold(
                    (0, 0),
                    |(super_count, as_count), node| {
                        if let TypeConstraint(c) = &node.children {
                            match token_kind(&c.keyword) {
                                Some(TokenKind::As) => return (super_count, as_count + 1),
                                Some(TokenKind::Super) => return (super_count + 1, as_count),
                                _ => (),
                            }
                        }
                        (super_count, as_count)
                    },
                );
                if super_count != 0 {
                    self.check_can_use_feature(node, &FeatureName::TypeConstSuperBound);
                }
                if super_count > 1 || as_count > 1 {
                    self.check_can_use_feature(node, &FeatureName::TypeConstMultipleBounds);
                }
            }
        }
    }

    fn type_refinement_errors(&mut self, node: S<'a>) {
        #[derive(Eq, PartialEq, Hash)]
        enum MemberKind {
            Type,
            Ctx,
        }
        fn member_id<'a>(member: S<'a>) -> Option<(MemberKind, S<'a>)> {
            match &member.children {
                TypeInRefinement(m) => Some((MemberKind::Type, &m.name)),
                CtxInRefinement(m) => Some((MemberKind::Ctx, &m.name)),
                _ => None,
            }
        }
        fn member_bounded<'a>(member: S<'a>) -> bool {
            let nonempty_constraints = |cs| syntax_to_list_no_separators(cs).next().is_some();
            match &member.children {
                TypeInRefinement(m) => !m.type_.is_missing() ^ nonempty_constraints(&m.constraints),
                CtxInRefinement(m) => {
                    !m.ctx_list.is_missing() ^ nonempty_constraints(&m.constraints)
                }
                _ => false, /* unreachable */
            }
        }
        if let TypeRefinement(r) = &node.children {
            // TODO(type-refinements): err when type_parameters is non-empty.
            // Alternatively, we can just avoid parsing them in the first time.
            let mut seen_members = HashSet::default();
            for member in syntax_to_list_no_separators(&r.members) {
                if let Some((kind, node)) = member_id(member) {
                    let name = self.text(node);
                    if !seen_members.insert((kind, name)) {
                        self.errors.push(make_error_from_node(
                            member,
                            errors::duplicate_refinement_member_of(name),
                        ))
                    }
                }
                if !member_bounded(member) {
                    self.errors.push(make_error_from_node(
                        member,
                        errors::unbounded_refinement_member_of(self.text(&r.type_)),
                    ));
                }
            }
        }
    }

    fn alias_errors(&mut self, node: S<'a>) {
        if let AliasDeclaration(ad) = &node.children {
            let attrs = &ad.attribute_spec;
            self.check_attrs(attrs);
            // Module newtype errors
            if !ad.module_kw_opt.is_missing() {
                if !self.in_module {
                    self.errors.push(make_error_from_node(
                        &ad.module_kw_opt,
                        errors::module_newtype_outside_of_module,
                    ));
                }
            }
            self.invalid_modifier_errors("Type aliases", node, |kind| {
                kind == TokenKind::Internal || kind == TokenKind::Public
            });
            if token_kind(&ad.keyword) == Some(TokenKind::Type) && !ad.constraint.is_missing() {
                self.errors
                    .push(make_error_from_node(&ad.keyword, errors::error2034))
            }

            if token_kind(&ad.keyword) == Some(TokenKind::Newtype) {
                for n in ad.constraint.syntax_node_to_list_skip_separator() {
                    if let TypeConstraint(c) = &n.children {
                        if let Some(TokenKind::Super) = token_kind(&c.keyword) {
                            self.check_can_use_feature(n, &FeatureName::NewtypeSuperBounds);
                            break;
                        }
                    };
                }
            }
            if !ad.name.is_missing() {
                let name = self.text(&ad.name);
                let location = make_location_of_node(&ad.name);
                if let TypeConstant(_) = &ad.type_.children {
                    if self.env.is_typechecker() {
                        self.errors.push(make_error_from_node(
                            &ad.type_,
                            errors::type_alias_to_type_constant,
                        ))
                    }
                }

                self.check_alias_name(&ad.name, name, location)
            }
        } else if let ContextAliasDeclaration(cad) = &node.children {
            if cad.equal.is_missing() {
                // short newctx X as []; syntax
                self.check_can_use_feature(node, &FeatureName::ContextAliasDeclarationShort);
            } else {
                self.check_can_use_feature(node, &FeatureName::ContextAliasDeclaration);
            }
            let attrs = &cad.attribute_spec;
            self.check_attrs(attrs);
            if token_kind(&cad.keyword) == Some(TokenKind::Type) && !cad.as_constraint.is_missing()
            {
                self.errors
                    .push(make_error_from_node(&cad.keyword, errors::error2034))
            }
            if !cad.name.is_missing() {
                let name = self.text(&cad.name);
                let location = make_location_of_node(&cad.name);
                if let TypeConstant(_) = &cad.context.children {
                    if self.env.is_typechecker() {
                        self.errors.push(make_error_from_node(
                            &cad.context,
                            errors::type_alias_to_type_constant,
                        ))
                    }
                }

                self.check_alias_name(&cad.name, name, location)
            }
        } else if let CaseTypeDeclaration(ctd) = &node.children {
            self.check_can_use_feature(node, &FeatureName::CaseTypes);

            let attrs = &ctd.attribute_spec;
            self.check_attrs(attrs);

            self.invalid_modifier_errors("Case types", node, |kind| {
                kind == TokenKind::Internal || kind == TokenKind::Public
            });

            if !ctd.name.is_missing() {
                let name = self.text(&ctd.name);
                let location = make_location_of_node(&ctd.name);

                self.check_alias_name(&ctd.name, name, location)
            }
        }
    }

    fn case_type_variant_errors(&mut self, node: S<'a>) {
        if let CaseTypeVariant(ctv) = &node.children {
            if let TypeConstant(_) = &ctv.type_.children {
                if self.env.is_typechecker() {
                    self.errors.push(make_error_from_node(
                        &ctv.type_,
                        errors::type_alias_to_type_constant,
                    ))
                }
            }
            if !ctv.where_clause.is_missing() {
                self.check_can_use_feature(&ctv.where_clause, &FeatureName::CaseTypeWhereClauses);
            }
        }
    }

    fn group_use_errors(&mut self, node: S<'a>) {
        if let NamespaceGroupUseDeclaration(x) = &node.children {
            let prefix = &x.prefix;
            let clauses = &x.clauses;
            let kind = &x.kind;
            syntax_to_list_no_separators(clauses)
                .filter(|x| is_invalid_group_use_clause(kind, x))
                .for_each(|clause| {
                    self.errors
                        .push(make_error_from_node(clause, errors::error2049))
                });
            self.produce_error(
                |_, x| is_invalid_group_use_prefix(x),
                prefix,
                || errors::error2048,
                prefix,
            )
        }
    }

    fn use_class_or_namespace_clause_errors(
        &mut self,
        namespace_prefix: Option<&str>,

        kind: S<'a>,
        cl: S<'a>,
    ) {
        match &cl.children {
            NamespaceUseClause(x) if !&x.name.is_missing() => {
                let name = &x.name;

                let kind = if kind.is_missing() {
                    &x.clause_kind
                } else {
                    kind
                };

                let name_text = self.text(name);
                let qualified_name = match namespace_prefix {
                    None => combine_names(GLOBAL_NAMESPACE_NAME, name_text),
                    Some(p) => combine_names(p, name_text),
                };
                let short_name = get_short_name_from_qualified_name(name_text, self.text(&x.alias));

                let do_check =
                    |self_: &mut Self,
                     error_on_global_redefinition,
                     get_names: &dyn Fn(&mut UsedNames) -> &mut Strmap<FirstUseOrDef>,
                     report_error| {
                        let is_global_namespace = self_.is_global_namespace();
                        let names = get_names(&mut self_.names);
                        match names.get(short_name) {
                            Some(FirstUseOrDef {
                                location,
                                kind,
                                global,
                                ..
                            }) => {
                                if *kind != NameDef
                                    || error_on_global_redefinition
                                        && (is_global_namespace || *global)
                                {
                                    self_.errors.push(make_name_already_used_error(
                                        name,
                                        name_text,
                                        short_name,
                                        location,
                                        report_error,
                                    ))
                                }
                            }
                            None => {
                                let new_use = make_first_use_or_def(
                                    false,
                                    NameUse,
                                    make_location_of_node(name),
                                    GLOBAL_NAMESPACE_NAME,
                                    &qualified_name,
                                );
                                names.add(short_name, new_use)
                            }
                        }
                    };

                match &kind.children {
                    Token(token) => match token.kind() {
                        TokenKind::Namespace => do_check(
                            self,
                            false,
                            &|x: &mut UsedNames| &mut x.namespaces,
                            &errors::namespace_name_is_already_in_use,
                        ),

                        TokenKind::Type => do_check(
                            self,
                            false,
                            &|x: &mut UsedNames| &mut x.classes,
                            &errors::type_name_is_already_in_use,
                        ),

                        TokenKind::Function => do_check(
                            self,
                            true,
                            &|x: &mut UsedNames| &mut x.functions,
                            &errors::function_name_is_already_in_use,
                        ),

                        TokenKind::Const => do_check(
                            self,
                            true,
                            &|x: &mut UsedNames| &mut x.constants,
                            &errors::const_name_is_already_in_use,
                        ),
                        _ => {}
                    },
                    Missing => {
                        if name_text == "strict" {
                            self.errors
                                .push(make_error_from_node(name, errors::strict_namespace_hh))
                        }
                        let location = make_location_of_node(name);

                        match self.names.classes.get(short_name) {
                            Some(FirstUseOrDef {
                                location: loc,
                                name: def_name,
                                kind,
                                ..
                            }) => {
                                if &qualified_name != def_name || kind != &NameDef {
                                    let (line_num, _) =
                                        self.env.text.offset_to_position(loc.start_offset as isize);
                                    let err_msg = |x: &str, y: &str| -> Error {
                                        if kind != &NameDef {
                                            if kind == &NameImplicitUse {
                                                errors::name_is_already_in_use_implicit_hh(
                                                    line_num, x, y,
                                                )
                                            } else {
                                                errors::name_is_already_in_use_hh(line_num, x, y)
                                            }
                                        } else {
                                            errors::name_is_already_in_use_php(x, y)
                                        }
                                    };

                                    self.errors.push(make_name_already_used_error(
                                        name, name_text, short_name, loc, &err_msg,
                                    ))
                                }
                            }
                            None => {
                                let new_use = make_first_use_or_def(
                                    false,
                                    NameUse,
                                    location,
                                    GLOBAL_NAMESPACE_NAME,
                                    &qualified_name,
                                );

                                if !self.names.namespaces.mem(short_name) {
                                    self.names.namespaces.add(short_name, new_use.clone());
                                    self.names.classes.add(short_name, new_use);
                                } else {
                                    self.names.classes.add(short_name, new_use);
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }

            _ => {}
        }
    }

    fn namespace_use_declaration_errors(&mut self, node: S<'a>) {
        match &node.children {
            NamespaceUseDeclaration(x) => {
                syntax_to_list_no_separators(&x.clauses).for_each(|clause| {
                    self.use_class_or_namespace_clause_errors(None, &x.kind, clause)
                })
            }
            NamespaceGroupUseDeclaration(x) => {
                syntax_to_list_no_separators(&x.clauses).for_each(|clause| {
                    match &clause.children {
                        NamespaceUseClause(x) if !x.name.is_missing() => {
                            self.check_preceding_backslashes_qualified_name(&x.name)
                        }
                        _ => {}
                    }
                    self.use_class_or_namespace_clause_errors(
                        Some(self.text(&x.prefix)),
                        &x.kind,
                        clause,
                    )
                })
            }
            _ => {}
        }
    }

    fn token_text(&self, token: &PositionedToken<'a>) -> &'a str {
        self.env.text.source_text().sub_as_str(
            token.leading_start_offset().unwrap() + token.leading_width(),
            token.width(),
        )
    }

    fn check_constant_expression_ban_static(&mut self, node: S<'a>) {
        self.check_constant_expression(node, LateBoundRule::NoStatic)
    }

    /// Whether `name`, as in `name::FOO` or `nameof name`, is `static`, or `self` or `parent` in a
    /// trait. Case-insensitive, as the lowerer resolves `nameof Self` and `nameof Parent`.
    fn is_late_bound_class_name(&self, name: S<'a>) -> bool {
        let text = self.text(name);
        text.eq_ignore_ascii_case(sn::classes::STATIC)
            || (self.active_classish_kind() == Some(TokenKind::Trait)
                && (text.eq_ignore_ascii_case(sn::classes::SELF)
                    || text.eq_ignore_ascii_case(sn::classes::PARENT)))
    }

    /// Checks whether this expression is valid in a constant position, such as a property initializer.
    /// For example:
    ///
    /// ```
    /// vec[vec["foo"]] // allowed
    /// UNSAFE_CAST<?int, int>(NULLABLE_CONST) // allowed
    /// foo() // not allowed
    /// ```
    ///
    /// `rule` sets which late-bound class references, like `static::class`, are considered constant.
    fn check_constant_expression(&mut self, node: S<'a>, rule: LateBoundRule) {
        // __FUNCTION_CREDENTIAL__ emits an object,
        // so it cannot be used in a constant expression
        let not_function_credential = |self_: &Self, token: &PositionedToken<'a>| {
            !self_
                .token_text(token)
                .eq_ignore_ascii_case("__FUNCTION_CREDENTIAL__")
        };

        let is_whitelisted_function = |self_: &Self, receiver_token| {
            let text = self_.text(receiver_token);
            (text == sn::std_lib_functions::ARRAY_MARK_LEGACY)
                || (text == strip_ns(sn::std_lib_functions::ARRAY_MARK_LEGACY))
                || (text == sn::std_lib_functions::ARRAY_UNMARK_LEGACY)
                || (text == strip_ns(sn::std_lib_functions::ARRAY_UNMARK_LEGACY))
                || (text == sn::pseudo_functions::UNSAFE_CAST)
                || (text == strip_ns(sn::pseudo_functions::UNSAFE_CAST))
                || (text == sn::pseudo_functions::UNSAFE_NONNULL_CAST)
                || (text == strip_ns(sn::pseudo_functions::UNSAFE_NONNULL_CAST))
        };

        let is_namey = |self_: &Self, token: &PositionedToken<'a>| -> bool {
            token.kind() == TokenKind::Name && not_function_credential(self_, token)
        };

        let is_good_scope_resolution_name = |node: S<'a>| match &node.children {
            QualifiedName(_) => true,
            Token(token) => {
                use TokenKind::*;
                match token.kind() {
                    Name | Trait | Extends | Implements | Static | Abstract | Final | Private
                    | Protected | Public | Global | Instanceof | Insteadof | Interface
                    | Namespace | New | Try | Use | Var | List | Clone | Include | Include_once
                    | Throw | Tuple | Print | Echo | Require | Require_once | Return | Else
                    | Default | Break | Continue | Switch | Yield | Function | If | Finally
                    | For | Foreach | Case | Do | While | As | Catch | Empty | Using | Class
                    | NullLiteral | Super | Where => true,
                    _ => false,
                }
            }
            _ => false,
        };

        let default = |self_: &mut Self| {
            self_.errors.push(make_error_from_node(
                node,
                errors::invalid_constant_initializer,
            ))
        };

        let check_type_specifier = |self_: &mut Self, x: S<'a>, initializer| {
            if let Token(token) = &x.children {
                if is_namey(self_, token) {
                    return syntax_to_list_no_separators(initializer)
                        .for_each(|x| self_.check_constant_expression(x, rule));
                }
            };
            default(self_)
        };

        let check_collection_members = |self_: &mut Self, x| {
            syntax_to_list_no_separators(x).for_each(|x| self_.check_constant_expression(x, rule))
        };
        match &node.children {
            Missing | QualifiedName(_) => {}
            LiteralExpression(y) => {
                if let SyntaxList(_) = &y.expression.children {
                    default(self)
                }
            }
            Token(token) => {
                if !is_namey(self, token) {
                    default(self)
                }
            }
            PrefixUnaryExpression(x) => {
                if let Token(token) = &x.operator.children {
                    use TokenKind::*;
                    match token.kind() {
                        Exclamation | Plus | Minus | Tilde => {
                            self.check_constant_expression(&x.operand, rule)
                        }
                        _ => default(self),
                    }
                } else {
                    default(self)
                }
            }
            UpcastExpression(x) => self.check_constant_expression(&x.left_operand, rule),
            BinaryExpression(x) => {
                if let Token(token) = &x.operator.children {
                    use TokenKind::*;
                    match token.kind() {
                        BarBar
                        | AmpersandAmpersand
                        | Carat
                        | Bar
                        | Ampersand
                        | Dot
                        | Plus
                        | Minus
                        | Star
                        | Slash
                        | Percent
                        | LessThanLessThan
                        | GreaterThanGreaterThan
                        | StarStar
                        | EqualEqual
                        | EqualEqualEqual
                        | ExclamationEqual
                        | ExclamationEqualEqual
                        | GreaterThan
                        | GreaterThanEqual
                        | LessThan
                        | LessThanEqual
                        | LessThanEqualGreaterThan
                        | QuestionColon => {
                            self.check_constant_expression(&x.left_operand, rule);
                            self.check_constant_expression(&x.right_operand, rule);
                        }
                        _ => default(self),
                    }
                } else {
                    default(self)
                }
            }
            ConditionalExpression(x) => {
                self.check_constant_expression(&x.test, rule);
                self.check_constant_expression(&x.consequence, rule);
                self.check_constant_expression(&x.alternative, rule);
            }
            SimpleInitializer(x) => self.check_constant_expression(&x.value, rule),
            ParenthesizedExpression(x) => self.check_constant_expression(&x.expression, rule),
            CollectionLiteralExpression(x) => {
                if let SimpleTypeSpecifier(y) = &x.name.children {
                    check_type_specifier(self, &y.specifier, &x.initializers)
                } else if let GenericTypeSpecifier(y) = &x.name.children {
                    check_type_specifier(self, &y.class_type, &x.initializers)
                } else {
                    default(self)
                };
            }

            TupleExpression(x) => check_collection_members(self, &x.items),
            KeysetIntrinsicExpression(x) => check_collection_members(self, &x.members),
            VarrayIntrinsicExpression(x) => check_collection_members(self, &x.members),
            DarrayIntrinsicExpression(x) => check_collection_members(self, &x.members),
            VectorIntrinsicExpression(x) => check_collection_members(self, &x.members),
            DictionaryIntrinsicExpression(x) => check_collection_members(self, &x.members),
            ShapeExpression(x) => check_collection_members(self, &x.fields),
            ElementInitializer(x) => {
                self.check_constant_expression(&x.key, rule);
                self.check_constant_expression(&x.value, rule);
            }
            FieldInitializer(x) => {
                self.check_constant_expression(&x.name, rule);
                self.check_constant_expression(&x.value, rule);
            }
            ShapeSplatExpression(x) => self.check_constant_expression(&x.expression, rule),
            // Allow `ClassName::foo` in a constant, but don't allow `parent::class` or `static::foo`,
            // and only allow `self` and `parent` in a trait as `rule` permits.
            ScopeResolutionExpression(x)
                if is_good_scope_resolution_qualifier(&x.qualifier, false)
                    && is_good_scope_resolution_name(&x.name)
                    && !is_parent_class_access(&x.qualifier, &x.name)
                    && !(rule == LateBoundRule::NoLateBound
                        && self.is_late_bound_class_name(&x.qualifier)) => {}
            NameofExpression(x) => match x.target.children {
                // Match behavior for ScopeResolution expression when rhs is ::class
                QualifiedName(_) => {}
                Token(t) => {
                    // The parser does a weird clone fork to separate a name-token of "parent" vs.
                    // a token kind of parent when the following token is a :: for scope resolution
                    // I want to leave nameof flexible to have arbitrary expressions, so these are
                    // always name tokens.
                    let txt = self.token_text(&t);
                    if txt == sn::classes::PARENT
                        || txt == sn::classes::STATIC
                        || (rule == LateBoundRule::NoLateBound
                            && self.is_late_bound_class_name(&x.target))
                    {
                        default(self)
                    }
                }
                _ => default(self),
            },
            FunctionCallExpression(x) => {
                let mut check_receiver_and_arguments = |receiver| {
                    if is_whitelisted_function(self, receiver) {
                        for node in syntax_to_list_no_separators(&x.argument_list) {
                            self.check_constant_expression(node, rule)
                        }
                    } else {
                        default(self)
                    }
                };

                match &x.receiver.children {
                    Token(tok) if tok.kind() == TokenKind::Name => {
                        check_receiver_and_arguments(&x.receiver)
                    }
                    QualifiedName(_) => check_receiver_and_arguments(&x.receiver),
                    _ => default(self),
                }
            }
            FunctionPointerExpression(_) => default(self),
            ObjectCreationExpression(_) => {
                // We allow "enum class" constants to be initialized via new.
                if !self.is_in_enum_class() {
                    default(self);
                }
            }
            PrefixedStringExpression(x) => {
                // Allow prefixed strings (e.g., re'/foo/') in constant expressions
                self.check_constant_expression(&x.str, rule)
            }
            _ => default(self),
        }
    }

    fn const_decl_errors(&mut self, node: S<'a>) {
        if let ConstantDeclarator(cd) = &node.children {
            if self.constant_abstract_with_initializer(&cd.initializer) {
                self.check_can_use_feature(node, &FeatureName::ClassConstDefault);
            }

            self.produce_error(
                |self_, x| self_.constant_concrete_without_initializer(x),
                &cd.initializer,
                || errors::error2050,
                &cd.initializer,
            );

            self.check_constant_expression_ban_static(&cd.initializer);

            if !cd.name.is_missing() {
                let constant_name = self.text(&cd.name);
                let location = make_location_of_node(&cd.name);
                let def = make_first_use_or_def(
                    false,
                    NameDef,
                    location,
                    &self.namespace_name,
                    constant_name,
                );

                match (
                    self.names.constants.get(constant_name),
                    self.first_parent_class_name(),
                ) {
                    // Only error if this is inside a class
                    (Some(_), Some(class_name)) => {
                        let full_name = class_name.to_string() + "::" + constant_name;
                        self.errors.push(make_error_from_node(
                            node,
                            errors::redeclaration_error(&full_name),
                        ))
                    }
                    (Some(prev_def), None) if prev_def.kind != NameDef => {
                        let (line_num, _) = self
                            .env
                            .text
                            .offset_to_position(prev_def.location.start_offset as isize);
                        let line_num = line_num as usize;

                        self.errors.push(make_name_already_used_error(
                            &cd.name,
                            &combine_names(&self.namespace_name, constant_name),
                            constant_name,
                            &def.location,
                            &|x, y| errors::declared_name_is_already_in_use(line_num, x, y),
                        ))
                    }
                    _ => {}
                }
                self.names.constants.add(constant_name, def)
            }
        }
    }

    fn class_property_modifiers_errors(&mut self, node: S<'a>) {
        if let PropertyDeclaration(x) = &node.children {
            let property_modifiers = &x.modifiers;

            let abstract_static_props = self.env.parser_options.abstract_static_props;
            self.invalid_modifier_errors("Properties", node, |kind| {
                if kind == TokenKind::Abstract {
                    return abstract_static_props;
                }
                kind == TokenKind::Static
                    || kind == TokenKind::Private
                    || kind == TokenKind::Protected
                    || kind == TokenKind::Internal
                    || kind == TokenKind::Public
                    || kind == TokenKind::Readonly
            });

            self.produce_error(
                |_, x| !syntax_to_list_no_separators(x).any(is_visibility),
                property_modifiers,
                || errors::property_requires_visibility,
                node,
            );

            if self.env.parser_options.abstract_static_props {
                self.produce_error(
                    |_, n| has_modifier_abstract(n) && !has_modifier_static(n),
                    node,
                    || errors::abstract_instance_property,
                    node,
                );
            }

            if has_modifier_abstract(node) && has_modifier_private(node) {
                self.errors.push(make_error_from_node(
                    node,
                    errors::elt_abstract_private("properties"),
                ));
            }
        }
    }

    fn class_property_const_errors(&mut self, node: S<'a>) {
        if let PropertyDeclaration(x) = &node.children {
            if self.attr_spec_contains_const(&x.attribute_spec)
                && self.attribute_specification_contains(
                    &x.attribute_spec,
                    sn::user_attributes::LATE_INIT,
                )
            {
                // __LateInit together with const just doesn't make sense.
                self.errors
                    .push(make_error_from_node(node, errors::no_const_late_init_props))
            }
        }
    }

    fn class_property_declarator_errors(&mut self, node: S<'a>) {
        let check_decls = |self_: &mut Self,
                           f: &dyn Fn(S<'a>) -> bool,
                           error: errors::Error,
                           property_declarators| {
            syntax_to_list_no_separators(property_declarators).for_each(|decl| {
                if let PropertyDeclarator(x) = &decl.children {
                    if f(&x.initializer) {
                        self_.errors.push(make_error_from_node(node, error.clone()))
                    }
                }
            })
        };
        if let PropertyDeclaration(x) = &node.children {
            if self.env.parser_options.const_static_props && has_modifier_static(node) {
                if self.env.parser_options.abstract_static_props && has_modifier_abstract(node) {
                    check_decls(
                        self,
                        &|n| !n.is_missing(),
                        errors::abstract_prop_init,
                        &x.declarators,
                    )
                } else if self.attr_spec_contains_const(&x.attribute_spec) {
                    check_decls(
                        self,
                        &|n| n.is_missing(),
                        errors::const_static_prop_init,
                        &x.declarators,
                    )
                }
            }
        }
    }

    fn mixed_namespace_errors(&mut self, node: S<'a>) {
        match &node.children {
            NamespaceBody(x) => {
                let s = start_offset(&x.left_brace);
                let e = end_offset(&x.right_brace);
                if let NamespaceType::Unbracketed(Location {
                    start_offset,
                    end_offset,
                }) = self.namespace_type
                {
                    let child = Some(SyntaxError::make(
                        start_offset,
                        end_offset,
                        errors::error2057,
                        vec![],
                    ));
                    self.errors.push(SyntaxError::make_with_child_and_type(
                        child,
                        s,
                        e,
                        ErrorType::ParseError,
                        errors::error2052,
                        vec![],
                    ))
                }
            }
            NamespaceEmptyBody(x) => {
                let s = start_offset(&x.semicolon);
                let e = end_offset(&x.semicolon);
                if let NamespaceType::Bracketed(Location {
                    start_offset,
                    end_offset,
                }) = self.namespace_type
                {
                    let child = Some(SyntaxError::make(
                        start_offset,
                        end_offset,
                        errors::error2056,
                        vec![],
                    ));
                    self.errors.push(SyntaxError::make_with_child_and_type(
                        child,
                        s,
                        e,
                        ErrorType::ParseError,
                        errors::error2052,
                        vec![],
                    ))
                }
            }
            NamespaceDeclaration(x) => {
                let mut is_first_decl = true;
                let mut has_code_outside_namespace = false;

                if let [
                    Syntax {
                        children: Script(_),
                        ..
                    },
                    syntax_list,
                ] = self.parents.as_slice()
                {
                    if let SyntaxList(_) = syntax_list.children {
                        is_first_decl = false;
                        for decl in syntax_to_list_no_separators(syntax_list) {
                            match &decl.children {
                                MarkupSection(_) => {}
                                NamespaceUseDeclaration(_)
                                | FileAttributeSpecification(_)
                                | ModuleMembershipDeclaration(_) => {}
                                NamespaceDeclaration(_) => {
                                    is_first_decl = true;
                                    break;
                                }
                                _ => break,
                            }
                        }

                        has_code_outside_namespace = !(x.body.is_namespace_empty_body())
                            && syntax_to_list_no_separators(syntax_list).any(|decl| {
                                match &decl.children {
                                    MarkupSection(_) => false,
                                    NamespaceDeclaration(_)
                                    | FileAttributeSpecification(_)
                                    | ModuleMembershipDeclaration(_)
                                    | EndOfFile(_)
                                    | NamespaceUseDeclaration(_) => false,
                                    _ => true,
                                }
                            })
                    }
                }

                if !is_first_decl {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::namespace_decl_first_statement,
                    ))
                }
                if has_code_outside_namespace {
                    self.errors
                        .push(make_error_from_node(node, errors::code_outside_namespace))
                }
            }
            _ => {}
        }
    }

    fn enumerator_errors(&mut self, node: S<'a>) {
        if let Enumerator(x) = &node.children {
            if self.text(&x.name).eq_ignore_ascii_case("class") {
                self.errors
                    .push(make_error_from_node(node, errors::enum_elem_name_is_class))
            }
            self.check_constant_expression_ban_static(&x.value);
        }
    }

    fn enum_decl_errors(&mut self, node: S<'a>) {
        if let EnumDeclaration(x) = &node.children {
            let attrs = &x.attribute_spec;
            self.check_attrs(attrs);
            if self.attr_spec_contains_const(attrs) {
                self.errors.push(make_error_from_node(
                    node,
                    errors::no_const_interfaces_traits_enums_classes,
                ))
            }
            self.invalid_modifier_errors("Enums", node, |kind| {
                kind == TokenKind::Internal || kind == TokenKind::Public
            });
            if !x.name.is_missing() {
                let name = self.text(&x.name);
                let location = make_location_of_node(&x.name);
                self.check_use_type_name(&x.name, name, location);
                self.produce_error(
                    |_, x| cant_be_reserved_type_name(x),
                    name,
                    || errors::reserved_keyword_as_type_name(name),
                    &x.name,
                );

                if x.base.is_missing() {
                    // Create a zero width region to insert the new text.
                    let quickfix_start = end_offset(&x.name);
                    let quickfix_end = end_offset(&x.name);

                    self.errors.push(make_error_from_node_with_quickfix(
                        &x.name,
                        errors::enum_missing_base_type,
                        "Add `arraykey` base type",
                        quickfix_start,
                        quickfix_end,
                        ": arraykey",
                    ))
                }
            }
        }
    }

    /// Checks whether `loperand` is valid syntax to appear in the context represented by
    /// `lval_root`. This is meant to span any context in which we're mutating some value. See
    /// `LvalRoot` for an accounting of the different lvalue contexts in Hack.
    /// Notably, this includes `inout` expressions, which are lvalues by way of being assignment.
    fn check_lvalue_and_inout(&mut self, loperand: S<'a>, lval_root: LvalRoot, c: NestingContext) {
        let append_errors =
            |self_: &mut Self, node, error| self_.errors.push(make_error_from_node(node, error));

        let err = |self_: &mut Self, error| append_errors(self_, loperand, error);

        let check_unary_expression = |self_: &mut Self, op| match token_kind(op) {
            Some(TokenKind::At) | Some(TokenKind::Dollar) => {}
            _ => err(self_, errors::not_allowed_in_write("Unary expression")),
        };

        let check_variable = |self_: &mut Self, text| {
            if text == sn::special_idents::THIS {
                err(self_, errors::invalid_lval(lval_root))
            }
        };

        match &loperand.children {
            // There's some syntax that is never allowed as part of an `inout` expression, so we
            // match against it specially.
            // - `list( ... )` is nonsensical as the argument to an `inout` function; `inout`
            //   expressions must be both valid lvalues *and* rvalues: `list( ... )` is only valid
            //   as an lvalue.
            // - class member access (static or instance) is disallowed to avoid unexpected behavior
            //   around ref-counting and dangling references to class members.
            // - `shape(...)` to keep things simple
            MemberSelectionExpression(_)
            | ListExpression(_)
            | ScopeResolutionExpression(_)
            | ShapeExpression(_)
            | TupleExpression(_)
                if lval_root == LvalRoot::Inout =>
            {
                err(self, errors::fun_arg_invalid_arg)
            }
            ListExpression(_) if matches!(c, NestingContext::ShapeTuple) => {
                err(self, errors::fun_arg_list_in_shape)
            }
            ShapeExpression(_) | TupleExpression(_) if matches!(c, NestingContext::List) => {
                err(self, errors::fun_arg_shape_in_list)
            }
            ListExpression(x) => syntax_to_list_no_separators(&x.members)
                .for_each(|n| self.check_lvalue_and_inout(n, lval_root, NestingContext::List)),
            TupleExpression(x) => {
                self.check_can_use_feature(loperand, &FeatureName::ShapeAndTupleDestructuring);
                for item in syntax_to_list_no_separators(&x.items) {
                    match &item.children {
                        PrefixUnaryExpression(c)
                            if token_kind(&c.operator) == Some(TokenKind::Optional) =>
                        {
                            self.check_lvalue_and_inout(
                                &c.operand,
                                lval_root,
                                NestingContext::ShapeTuple,
                            )
                        }
                        _ => {
                            self.check_lvalue_and_inout(item, lval_root, NestingContext::ShapeTuple)
                        }
                    }
                }
            }
            ShapeExpression(x) => {
                self.check_can_use_feature(loperand, &FeatureName::ShapeAndTupleDestructuring);
                for field in syntax_to_list_no_separators(&x.fields) {
                    match &field.children {
                        FieldInitializer(x) => self.check_lvalue_and_inout(
                            &x.value,
                            lval_root,
                            NestingContext::ShapeTuple,
                        ),
                        // ?'key' => $var or ?$var (optional field in destructuring)
                        PrefixUnaryExpression(x)
                            if token_kind(&x.operator) == Some(TokenKind::Question) =>
                        {
                            match &x.operand.children {
                                FieldInitializer(fi) => self.check_lvalue_and_inout(
                                    &fi.value,
                                    lval_root,
                                    NestingContext::ShapeTuple,
                                ),
                                VariableExpression(_) => {} // ?$var punning, valid
                                _ => err(self, errors::not_allowed_in_write("Expression")),
                            }
                        }
                        ShapeSplatExpression(_) => {
                            append_errors(self, field, errors::invalid_lval(lval_root))
                        }
                        VariableExpression(_) => {} // $var punning, valid
                        _ => {}
                    }
                }
            }
            SafeMemberSelectionExpression(_) => {
                err(self, errors::not_allowed_in_write("`?->` operator"))
            }
            MemberSelectionExpression(x) => {
                if token_kind(&x.name) == Some(TokenKind::XHPClassName) {
                    err(self, errors::not_allowed_in_write("`->:` operator"));
                }
            }
            CatchClause(x) => check_variable(self, self.text(&x.variable)),
            VariableExpression(x) => {
                let txt = self.text(&x.expression);
                check_variable(self, txt);
                if lval_root == LvalRoot::Inout && sn::superglobals::is_any_global(txt) {
                    err(self, errors::fun_arg_invalid_arg);
                }
            }
            DecoratedExpression(x) => match token_kind(&x.decorator) {
                Some(TokenKind::Clone) => err(self, errors::not_allowed_in_write("`clone`")),
                Some(TokenKind::Await) => err(self, errors::not_allowed_in_write("`await`")),
                Some(TokenKind::Delay) => err(self, errors::not_allowed_in_write("`delay`")),
                Some(TokenKind::QuestionQuestion) => {
                    err(self, errors::not_allowed_in_write("`??` operator"))
                }
                Some(TokenKind::BarGreaterThan) => {
                    err(self, errors::not_allowed_in_write("`|>` operator"))
                }
                Some(TokenKind::BarQuestionGreaterThan) => {
                    err(self, errors::not_allowed_in_write("`|?>` operator"))
                }
                Some(TokenKind::Inout) => err(self, errors::not_allowed_in_write("`inout`")),
                _ => {}
            },
            ParenthesizedExpression(x) => self.check_lvalue_and_inout(&x.expression, lval_root, c),
            SubscriptExpression(x) => {
                self.check_lvalue_and_inout(&x.receiver, lval_root, c);
                if lval_root == LvalRoot::Inout && x.index.is_missing() {
                    err(self, errors::fun_arg_invalid_arg);
                }
            }
            PrefixUnaryExpression(x) => check_unary_expression(self, &x.operator),
            PostfixUnaryExpression(x) => check_unary_expression(self, &x.operator),
            // Potentially un-intuitive; `missing` is perfectly valid in LHS positions, for example:
            //
            // list( , $_) = ...;
            //      ^ this is "missing"
            //
            // $x[ ] = 42;
            //    ^ this is "missing", though we don't end up scanning for it in this function
            Missing => {}
            // A scope resolution expression is a valid lvalue if we're referencing a static
            // variable, otherwise it's nonsense.
            ScopeResolutionExpression(e) => {
                // Foo::$bar = ...;
                // OK, as this is a static member assignment
                if Some(TokenKind::Variable) == token_kind(&e.name) {
                    return;
                }
                // Foo::{_} = ...;
                // OK, as this is some form of static member assignment.
                if let BracedExpression(_) = e.name.children {
                    return;
                }
                // Foo::BAR = ...;  // Not OK, this is an assignment to a constant
                err(self, errors::invalid_lval(lval_root))
            }
            // Bare `_` is a valid wildcard in shape/tuple destructuring patterns
            Token(t)
                if matches!(c, NestingContext::ShapeTuple)
                    && t.kind() == TokenKind::Name
                    && self.text(loperand) == "_" => {}
            _ => err(self, errors::invalid_lval(lval_root)),
        }
    }

    /// `unset` only removes a container element. Unsetting a local, a property
    /// or a static property is no longer part of the language.
    fn check_unset_target(&mut self, target: S<'a>) {
        match &target.children {
            ParenthesizedExpression(x) => self.check_unset_target(&x.expression),
            // `?->` is rejected for a more specific reason than "not an element
            // access", and the general lvalue check always names it.
            SubscriptExpression(_) | SafeMemberSelectionExpression(_) => {
                self.check_lvalue_and_inout(target, LvalRoot::Unset, NestingContext::None)
            }
            _ => self
                .errors
                .push(make_error_from_node(target, errors::invalid_unset_target)),
        }
    }

    fn assignment_errors(&mut self, node: S<'a>) {
        let check_unary_expression = |self_: &mut Self, op, loperand: S<'a>| {
            if does_unop_create_write(token_kind(op)) {
                self_.check_lvalue_and_inout(
                    loperand,
                    LvalRoot::IncrementOrDecrement,
                    NestingContext::None,
                );
            }
        };
        match &node.children {
            PrefixUnaryExpression(x) => check_unary_expression(self, &x.operator, &x.operand),
            PostfixUnaryExpression(x) => check_unary_expression(self, &x.operator, &x.operand),
            DecoratedExpression(x) => {
                let loperand = &x.expression;
                if does_decorator_create_write(token_kind(&x.decorator)) {
                    self.check_lvalue_and_inout(loperand, LvalRoot::Inout, NestingContext::None)
                }
            }
            BinaryExpression(x) => {
                let loperand = &x.left_operand;
                if does_binop_create_write_on_left(token_kind(&x.operator)) {
                    self.check_lvalue_and_inout(
                        loperand,
                        LvalRoot::Assignment,
                        NestingContext::None,
                    );
                }
            }
            ForeachStatement(x) => {
                self.check_lvalue_and_inout(&x.value, LvalRoot::Foreach, NestingContext::None);
                self.check_lvalue_and_inout(&x.key, LvalRoot::Foreach, NestingContext::None);
            }
            CatchClause(_) => {
                self.check_lvalue_and_inout(node, LvalRoot::CatchClause, NestingContext::None);
            }
            _ => {}
        }
    }

    fn dynamic_method_call_errors(&mut self, node: S<'a>) {
        match &node.children {
            FunctionCallExpression(x) if !x.type_args.is_missing() => {
                let is_variable = |x| is_token_kind(x, TokenKind::Variable);
                let is_dynamic = match &x.receiver.children {
                    ScopeResolutionExpression(x) => is_variable(&x.name),
                    MemberSelectionExpression(x) => is_variable(&x.name),
                    SafeMemberSelectionExpression(x) => is_variable(&x.name),
                    _ => false,
                };
                if is_dynamic {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::no_type_parameters_on_dynamic_method_calls,
                    ))
                }
            }
            _ => {}
        }
    }

    fn get_namespace_name(&self) -> String {
        if let Some(node) = self.nested_namespaces.last() {
            if let NamespaceDeclaration(x) = &node.children {
                if let NamespaceDeclarationHeader(x) = &x.header.children {
                    let ns = &x.name;
                    if !ns.is_missing() {
                        return combine_names(&self.namespace_name, self.text(ns));
                    }
                }
            }
        }
        self.namespace_name.clone()
    }

    fn param_default_decl_errors(&mut self, node: S<'a>) {
        if let ParameterDeclaration(x) = &node.children {
            if parameter_modifier(node, TokenKind::Named).is_some() {
                self.check_can_use_feature(node, &FeatureName::NamedParameters);
            }
            self.check_constant_expression(&x.default_value, LateBoundRule::NoLateBound)
        }
    }

    fn concurrent_statement_errors(&mut self, node: S<'a>) {
        if let ConcurrentStatement(x) = &node.children {
            // issue error if concurrent blocks are nested
            if self.context.is_in_concurrent_block {
                self.errors
                    .push(make_error_from_node(node, errors::nested_concurrent_blocks))
            };
            if let CompoundStatement(x) = &x.statement.children {
                let statement_list = || syntax_to_list_no_separators(&x.statements);
                if statement_list().nth(1).is_none() {
                    self.errors.push(make_error_from_node(
                        node,
                        errors::fewer_than_two_statements_in_concurrent_block,
                    ))
                }
                for n in statement_list() {
                    if let ExpressionStatement(x) = &n.children {
                        if !self.node_has_await_child(&x.expression) {
                            self.errors.push(make_error_from_node(
                                n,
                                errors::statement_without_await_in_concurrent_block,
                            ))
                        }
                    } else {
                        self.errors.push(make_error_from_node(
                            n,
                            errors::invalid_syntax_concurrent_block,
                        ))
                    }
                }
                for n in statement_list() {
                    for error in self.find_invalid_lval_usage(n) {
                        self.errors.push(error)
                    }
                }
            } else {
                self.errors.push(make_error_from_node(
                    node,
                    errors::invalid_syntax_concurrent_block,
                ))
            }
        }
    }

    fn check_qualified_name(&mut self, node: S<'a>) {
        // The last segment in a qualified name should not have a trailing backslash
        // i.e. `Foospace\Bar\` except as the prefix of a GroupUseClause
        if let Some(Syntax {
            children: NamespaceGroupUseDeclaration(_),
            ..
        }) = self.parents.last()
        {
            // Ok
        } else if let QualifiedName(x) = &node.children {
            let name_parts = &x.parts;
            let mut parts = syntax_to_list_with_separators(name_parts);
            let last_part = parts.nth_back(0);
            match last_part {
                Some(t) if token_kind(t) == Some(TokenKind::Backslash) => {
                    self.errors.push(make_error_from_node(t, errors::error0008))
                }
                _ => {}
            }
        }
    }

    fn check_preceding_backslashes_qualified_name(&mut self, node: S<'a>) {
        // Qualified names as part of file level declarations
        // (group use, namespace use, namespace declarations) should not have preceding backslashes
        // `use namespace A\{\B}` will throw this error.
        if let QualifiedName(x) = &node.children {
            let name_parts = &x.parts;
            let mut parts = syntax_to_list_with_separators(name_parts);
            let first_part = parts.find(|x| !x.is_missing());

            match first_part {
                Some(t) if token_kind(t) == Some(TokenKind::Backslash) => self
                    .errors
                    .push(make_error_from_node(node, errors::preceding_backslash)),
                _ => {}
            }
        }
    }

    fn is_global_namespace(&self) -> bool {
        self.namespace_name == GLOBAL_NAMESPACE_NAME
    }

    fn folder(&mut self, node: S<'a>) {
        let mut prev_context = None;
        let mut pushed_nested_namespace = false;

        match self.parents.last() {
            Some(Syntax {
                children: BinaryExpression(x),
                ..
            }) if std::ptr::eq(node, &x.right_operand)
                && (token_kind(&x.operator) == Some(TokenKind::BarGreaterThan)
                    || token_kind(&x.operator) == Some(TokenKind::BarQuestionGreaterThan)) =>
            {
                prev_context = Some(self.env.context.clone());
                self.env.context.pipe_variable_state = PipeVariableState::Available;
            }
            _ => {}
        };

        match &node.children {
            ConstDeclaration(_) => {
                prev_context = Some(self.env.context.clone());
                self.env.context.active_const = Some(node)
            }
            FunctionDeclaration(x) => {
                self.named_function_context(node, &x.attribute_spec, &mut prev_context)
            }
            MethodishDeclaration(x) => {
                self.named_function_context(node, &x.attribute, &mut prev_context)
            }
            NamespaceDeclaration(x) => {
                if let NamespaceDeclarationHeader(x) = &x.header.children {
                    let namespace_name = &x.name;
                    if !namespace_name.is_missing() && !self.text(namespace_name).is_empty() {
                        pushed_nested_namespace = true;
                        self.nested_namespaces.push(node)
                    }
                }
            }
            AnonymousFunction(x) => self.lambda_context(node, &x.attribute_spec, &mut prev_context),
            LambdaExpression(x) => self.lambda_context(node, &x.attribute_spec, &mut prev_context),
            AwaitableCreationExpression(x) => {
                self.lambda_context(node, &x.attribute_spec, &mut prev_context)
            }
            ClassishDeclaration(_) => {
                prev_context = Some(self.env.context.clone());
                self.env.context.active_classish = Some(node)
            }
            TypeRefinement(_) => {
                self.type_refinement_errors(node);
            }
            FileAttributeSpecification(_) => self.file_attribute_spec(node),
            ModuleDeclaration(_) => {}
            ModuleMembershipDeclaration(_) => {
                self.in_module = true;
            }
            EnumClassDeclaration(_) => {
                prev_context = Some(self.env.context.clone());
                self.env.context.active_enum_class = Some(node);
            }
            _ => {}
        };

        self.parameter_errors(node);

        match &node.children {
            TryStatement(_)
            | UsingStatementFunctionScoped(_)
            | ForStatement(_)
            | CaseLabel(_)
            | DefaultLabel(_) => self.statement_errors(node),
            ReturnStatement(x)
                if !x.expression.is_missing() && self.env.context.active_callable_is_generator =>
            {
                self.errors
                    .push(make_error_from_node(node, errors::return_in_generator))
            }
            MethodishDeclaration(_) | FunctionDeclaration(_) | FunctionDeclarationHeader(_) => {
                self.reified_parameter_errors(node);
                self.redeclaration_errors(node);
                self.multiple_entrypoint_attribute_errors(node);
                self.methodish_errors(node);
            }

            LiteralExpression(_)
            | SafeMemberSelectionExpression(_)
            | FunctionCallExpression(_)
            | ListExpression(_)
            | ShapeExpression(_)
            | DecoratedExpression(_)
            | VectorIntrinsicExpression(_)
            | DictionaryIntrinsicExpression(_)
            | KeysetIntrinsicExpression(_)
            | VarrayIntrinsicExpression(_)
            | DarrayIntrinsicExpression(_)
            | YieldExpression(_)
            | ScopeResolutionExpression(_)
            | PrefixUnaryExpression(_)
            | LambdaExpression(_)
            | IsExpression(_)
            | AsExpression(_)
            | AnonymousFunction(_)
            | SubscriptExpression(_)
            | ConstructorCall(_)
            | AwaitableCreationExpression(_)
            | PipeVariableExpression(_)
            | ConditionalExpression(_)
            | CollectionLiteralExpression(_)
            | ETSpliceExpression(_)
            | VariableExpression(_) => {
                // Subscript reads inside an expression tree are gated behind the
                // `expression_tree_subscript` feature. Everything below this is the
                // shared handling for all the expression kinds in this arm.
                if self.context.active_expression_tree
                    && matches!(&node.children, SubscriptExpression(_))
                {
                    self.check_can_use_feature(node, &FeatureName::ExpressionTreeSubscript);
                }
                self.check_disallowed_variables(node);
                self.dynamic_method_call_errors(node);
                self.expression_errors(node);
                self.assignment_errors(node);
            }

            NamedArgument(_) => {}

            ParameterDeclaration(_) => self.param_default_decl_errors(node),
            RequireClause(_) => self.require_errors(node),
            RequireClauseConstraint(_) => {
                self.check_can_use_feature(node, &FeatureName::RequireConstraint)
            }
            ClassishDeclaration(children) => {
                self.classish_errors(node);
                self.class_reified_param_errors(node);
                self.check_type_parameters_attributes(&children.type_parameters);
            }

            EnumClassDeclaration(_) => {
                self.enum_class_errors(node);
            }

            EnumClassEnumerator(_) => self.enum_class_enumerator_errors(node),

            ConstDeclaration(_) => self.class_constant_modifier_errors(node),

            TypeConstDeclaration(_) => {
                self.type_const_modifier_errors(node);
                self.type_const_bounds_errors(node);
            }

            AliasDeclaration(_) | ContextAliasDeclaration(_) | CaseTypeDeclaration(_) => {
                self.alias_errors(node)
            }
            CaseTypeVariant(_) => self.case_type_variant_errors(node),
            ConstantDeclarator(_) => self.const_decl_errors(node),
            NamespaceBody(_) | NamespaceEmptyBody(_) | NamespaceDeclaration(_) => {
                self.mixed_namespace_errors(node)
            }
            NamespaceUseDeclaration(_) | NamespaceGroupUseDeclaration(_) => {
                self.group_use_errors(node);
                self.namespace_use_declaration_errors(node);
            }
            PropertyDeclaration(_) => {
                self.class_property_modifiers_errors(node);
                self.class_reified_param_errors(node);
                self.class_property_const_errors(node);
                self.class_property_declarator_errors(node);
            }
            EnumDeclaration(_) => self.enum_decl_errors(node),
            Enumerator(_) => self.enumerator_errors(node),
            BinaryExpression(x)
                if self.context.active_expression_tree
                    && token_kind(&x.operator) == Some(TokenKind::QuestionQuestion) =>
            {
                self.check_can_use_feature(node, &FeatureName::ExpressionTreeCoalesceOperator);
                self.assignment_errors(node);
            }
            PostfixUnaryExpression(_)
            | BinaryExpression(_)
            | ForeachStatement(_)
            | CatchClause(_) => self.assignment_errors(node),
            XHPEnumType(_) | XHPExpression(_) => self.xhp_errors(node),
            PropertyDeclarator(x) => self.check_constant_expression_ban_static(&x.initializer),
            // `static` here would resolve against whichever class first builds the cached attribute
            // declaration.
            XHPClassAttribute(x) => self.check_constant_expression_ban_static(&x.initializer),
            QualifiedName(_) => self.check_qualified_name(node),
            UnsetStatement(x) => {
                for expr in syntax_to_list_no_separators(&x.variables) {
                    self.check_unset_target(expr);
                }
            }
            ShapeSplatSpecifier(_) => {
                // The type-level splat syntax `...T` is admitted by either the
                // concrete feature or the type-parameter feature. Whether `T` is
                // a type parameter (requiring `shape_splat_type_parameters`) is
                // a semantic question resolved after naming, in an elab pass.
                self.check_can_use_any_feature(
                    node,
                    &[
                        FeatureName::ShapeSplatConcrete,
                        FeatureName::ShapeSplatTypeParameters,
                    ],
                );
            }
            AbsentFieldSpecifier(_) => {
                // `absent 'x'` is sugar for `?'x' => nothing`; it is part of the
                // concrete shape normal form, gated by the concrete feature.
                self.check_can_use_feature(node, &FeatureName::ShapeSplatConcrete);
            }
            Token(t)
                if t.kind() == TokenKind::Name
                    && self.env.parser_options.enable_intrinsics_extension
                    && self.token_text(t) == "__EXPERIMENTAL_TEST_FEATURE_STATUS__" =>
            {
                self.check_can_use_feature(node, &FeatureName::TestFeature);
            }
            Token(t)
                if t.kind() == TokenKind::Variable
                    && self.token_text(t) == sn::special_idents::PROSECCO_TAP =>
            {
                self.errors
                    .push(make_error_from_node(node, errors::prosecco_tap_disallowed));
            }
            _ => {}
        }
        self.lval_errors(node);

        match &node.children {
            LambdaSignature(c) if !c.type_parameters.is_missing() => {
                self.check_can_use_feature(node, &FeatureName::PolymorphicLambda)
            }
            AnonymousFunction(children) if !children.type_parameters.is_missing() => {
                self.check_can_use_feature(node, &FeatureName::PolymorphicLambda);
                self.lambda(node)
            }
            // todo: lambda
            LambdaExpression(_) | AwaitableCreationExpression(_) | AnonymousFunction(_) => {
                self.lambda(node)
            }
            ConcurrentStatement(_) => self.concurrent_stmt(node),
            NamespaceBody(x) => self.namespace_body(node, &x.left_brace, &x.right_brace),
            NamespaceEmptyBody(x) => self.namespace_empty_body(node, &x.semicolon),
            ClassishDeclaration(_) | AnonymousClass(_) => self.classes(node),
            PrefixedCodeExpression(x) => {
                self.prefixed_code_expr(node, &x.prefix, &mut prev_context)
            }
            ETSpliceExpression(_) => self.et_splice_expr(node),
            _ => self.fold_child_nodes(node),
        }

        match &node.children {
            ClosureTypeSpecifier(c) if !c.type_parameters.is_missing() => {
                self.check_can_use_feature(node, &FeatureName::PolymorphicFunctionHints)
            }
            UnionTypeSpecifier(_) | IntersectionTypeSpecifier(_) => {
                self.check_can_use_feature(node, &FeatureName::UnionIntersectionTypeHints)
            }
            LikeTypeSpecifier(_) if !self.env.hhi_mode => {
                self.check_can_use_feature(node, &FeatureName::LikeTypeHints)
            }
            UpcastExpression(_) => self.check_can_use_feature(node, &FeatureName::UpcastExpression),
            OldAttributeSpecification(x) => {
                self.old_attr_spec(node, &x.attributes);
            }
            MatchStatement(_) => {
                self.check_can_use_feature(node, &FeatureName::MatchStatements);
            }
            ClassAliasDeclaration(_) => {
                self.check_can_use_feature(node, &FeatureName::ClassAliasesEverywhere);
            }
            _ => {}
        }

        if let Some(prev_context) = prev_context {
            self.env.context = prev_context;
        }
        if pushed_nested_namespace {
            self.check_nested_namespace(node);
        }
    }

    fn file_attribute_spec(&mut self, node: S<'a>) {
        for node in attr_spec_to_node_list(node) {
            match self.attr_name(node) {
                Some(sn::user_attributes::ENABLE_UNSTABLE_FEATURES) => {
                    if let Some(args) = self.attr_args(node) {
                        let mut args = args.peekable();
                        if args.peek().is_none() {
                            self.errors.push(make_error_from_node(
                            node,
                            errors::invalid_use_of_enable_unstable_feature(
                                format!(
                                    "you didn't select a feature. Available features are:\n\t{}",
                                    FeatureName::iter().join("\n\t")
                                )
                                .as_str(),
                            ),
                        ))
                        } else {
                            args.for_each(|arg| self.enable_experimental_feature(node, arg))
                        }
                    }
                }
                Some(sn::user_attributes::PACKAGE_OVERRIDE) => {
                    if let Some(args) = self.attr_args(node) {
                        let mut count = 0;
                        args.for_each(|_| count += 1);
                        if count != 1 {
                            self.errors
                                .push(make_error_from_node(node, errors::invalid_package_override))
                        }
                    }
                }
                Some(_) | None => {}
            }
        }
    }

    fn node_contains_yield(node: S<'a>) -> bool {
        let mut worklist: Vec<S<'a>> = vec![node];
        while let Some(n) = worklist.pop() {
            match &n.children {
                YieldExpression(_) => return true,
                AnonymousFunction(_)
                | LambdaExpression(_)
                | AwaitableCreationExpression(_)
                | FunctionDeclaration(_)
                | MethodishDeclaration(_) => {}
                _ => worklist.extend(n.iter_children()),
            }
        }
        false
    }

    fn callable_body_is_generator(node: S<'a>) -> bool {
        match &node.children {
            FunctionDeclaration(x) => Self::node_contains_yield(&x.body),
            MethodishDeclaration(x) => Self::node_contains_yield(&x.function_body),
            AnonymousFunction(x) => Self::node_contains_yield(&x.body),
            LambdaExpression(x) => Self::node_contains_yield(&x.body),
            AwaitableCreationExpression(x) => Self::node_contains_yield(&x.compound_statement),
            _ => false,
        }
    }

    fn named_function_context(
        &mut self,
        node: S<'a>,
        s: S<'a>,
        prev_context: &mut Option<Context<'a>>,
    ) {
        *prev_context = Some(self.env.context.clone());
        // a _single_ variable suffices as they cannot be nested
        self.env.context.active_methodish = Some(node);
        self.env.context.active_callable = Some(node);
        self.env.context.active_callable_attr_spec = Some(s);
        self.env.context.active_callable_is_generator = Self::callable_body_is_generator(node);
    }

    fn lambda_context(&mut self, node: S<'a>, s: S<'a>, prev_context: &mut Option<Context<'a>>) {
        *prev_context = Some(self.env.context.clone());
        // preserve context when entering lambdas (and anonymous functions)
        self.env.context.active_callable = Some(node);
        self.env.context.active_callable_attr_spec = Some(s);
        self.env.context.active_callable_is_generator = Self::callable_body_is_generator(node);

        // ExpressionTrees don't support pipe expressions but if a pipe-variable
        // is captured by an expression tree it can always be used in any lambdas
        // therein regardless of whether pipe variable capture is enabled.
        if !self.context.active_expression_tree {
            self.env.context.pipe_variable_state = match self.env.context.pipe_variable_state {
                PipeVariableState::Available | PipeVariableState::Captured => {
                    PipeVariableState::Captured
                }
                PipeVariableState::Unavailable => PipeVariableState::Unavailable,
            };
        };
    }

    fn reset_parser_error_context_for_lambda(&mut self) -> ParserErrorsContext {
        if self.context.active_expression_tree {
            self.context.clone()
        } else {
            std::mem::take(&mut self.context)
        }
    }

    fn lambda(&mut self, node: S<'a>) {
        // reset the context to default for functions, and restore it at the end
        let prev_context = self.reset_parser_error_context_for_lambda();
        // analyze the body of lambda block
        self.fold_child_nodes(node);
        // adjust is_in_concurrent_block in final result
        self.context = prev_context;
    }

    fn concurrent_stmt(&mut self, node: S<'a>) {
        self.concurrent_statement_errors(node);
        // adjust is_in_concurrent_block in accumulator to dive into the
        let prev_is_in_concurrent_block = self.context.is_in_concurrent_block;
        self.context.is_in_concurrent_block = true;
        // analyze the body of concurrent block
        self.fold_child_nodes(node);
        // adjust is_in_concurrent_block in final result
        self.context.is_in_concurrent_block = prev_is_in_concurrent_block;
    }

    fn namespace_body(&mut self, node: S<'a>, left_brace: S<'a>, right_brace: S<'a>) {
        if self.namespace_type == Unspecified {
            self.namespace_type = Bracketed(make_location(left_brace, right_brace))
        }

        let old_namespace_name = self.namespace_name.clone();
        let old_names = self.names.clone();
        // reset names before diving into namespace body,
        // keeping global function names
        self.namespace_name = self.get_namespace_name();
        let names_copy = std::mem::replace(&mut self.names, UsedNames::empty());
        self.names.functions = names_copy.functions.filter(|x| x.global);
        self.fold_child_nodes(node);

        // resume with old set of names and pull back
        // accumulated errors/last seen namespace type
        self.names = old_names;
        self.namespace_name = old_namespace_name;
    }

    fn namespace_empty_body(&mut self, node: S<'a>, semi: S<'a>) {
        if self.namespace_type == Unspecified {
            self.namespace_type = Unbracketed(make_location_of_node(semi))
        }
        self.namespace_name = self.get_namespace_name();
        self.names = UsedNames::empty();
        self.fold_child_nodes(node);
    }

    fn classes(&mut self, node: S<'a>) {
        // Reset the trait require clauses
        // Reset the const declarations
        // Reset the function declarations

        let constants = std::mem::replace(&mut self.names.constants, ConstCase(HashMap::default()));
        let functions = std::mem::replace(&mut self.names.functions, FuncCase(HashMap::default()));
        let trait_require_clauses = std::mem::replace(
            &mut self.trait_require_clauses,
            empty_trait_require_clauses(),
        );

        self.fold_child_nodes(node);

        self.trait_require_clauses = trait_require_clauses;
        self.names.functions = functions;
        self.names.constants = constants;
    }

    fn prefixed_code_expr(
        &mut self,
        node: S<'a>,
        prefix: S<'a>,
        prev_context: &mut Option<Context<'a>>,
    ) {
        if self.context.expression_tree_depth == 0 && prefix.is_missing() {
            self.errors
                .push(make_error_from_node(node, errors::expression_tree_name))
        } else if self.context.expression_tree_depth != 0
            && prefix.is_missing()
            && !self.context.active_expression_tree
        {
            self.errors
                .push(make_error_from_node(prefix, errors::expression_tree_name))
        }

        *prev_context = Some(self.env.context.clone());
        self.context.active_expression_tree = true;
        self.context.expression_tree_depth += 1;
        self.fold_child_nodes(node);
        self.context.active_expression_tree = false;
        self.context.expression_tree_depth -= 1;
    }

    fn et_splice_expr(&mut self, node: S<'a>) {
        if self.context.expression_tree_depth > 0 && !self.context.active_expression_tree {
            self.errors.push(make_error_from_node(
                node,
                "Splice syntax `${...}` cannot be nested.".into(),
            ))
        }
        let previous_state = self.context.active_expression_tree;
        self.context.active_expression_tree = false;
        self.fold_child_nodes(node);
        self.context.active_expression_tree = previous_state;
    }

    fn old_attr_spec(&mut self, node: S<'a>, attributes: S<'a>) {
        let attributes = self.text(attributes).split(',');
        attributes.for_each(|attr| match attr.trim() {
            sn::user_attributes::SIMPLIHACK => {
                self.check_can_use_feature(node, &FeatureName::SimpliHack)
            }
            _ => {}
        });
    }

    fn check_nested_namespace(&mut self, node: S<'a>) {
        assert_eq!(
            self.nested_namespaces.pop().map(|x| x as *const _),
            Some(node as *const _)
        );
    }

    fn fold_child_nodes(&mut self, node: S<'a>) {
        stack_limit::maybe_grow(|| {
            self.parents.push(node);
            for c in node.iter_children() {
                self.folder(c);
            }
            assert_eq!(
                self.parents.pop().map(|x| x as *const _),
                Some(node as *const _)
            );
        })
    }

    fn parse_errors_impl(mut self) -> (Vec<SyntaxError>, bool, HashSet<FeatureName>) {
        self.fold_child_nodes(self.env.syntax_tree.root());
        self.errors.reverse();
        (
            self.errors,
            self.uses_readonly,
            self.env.context.active_experimental_features,
        )
    }

    fn parse_errors(
        tree: &'a SyntaxTree<'a, Syntax<'a, PositionedToken<'a>, PositionedValue<'a>>, State>,
        text: IndexedSourceText<'a>,
        parser_options: ParserOptions,
        hhvm_compat_mode: bool,
        hhi_mode: bool,
        mode: Mode,
        systemlib: bool,
        default_experimental_features: HashSet<FeatureName>,
    ) -> (Vec<SyntaxError>, bool, HashSet<FeatureName>) {
        let env = Env {
            parser_options,
            syntax_tree: tree,
            text,
            context: Context {
                active_classish: None,
                active_methodish: None,
                active_callable: None,
                active_callable_attr_spec: None,
                active_const: None,
                active_enum_class: None,
                active_callable_is_generator: false,
                pipe_variable_state: PipeVariableState::Unavailable,
                active_experimental_features: default_experimental_features,
            },
            hhvm_compat_mode,
            hhi_mode,
            mode,
            systemlib,
        };
        Self::new(env).parse_errors_impl()
    }
}

enum NestingContext {
    None,
    List,
    ShapeTuple,
}

pub fn parse_errors<'a, State: Clone>(
    tree: &'a SyntaxTree<'a, PositionedSyntax<'a>, State>,
    parser_options: ParserOptions,
    hhvm_compat_mode: bool,
    hhi_mode: bool,
    ns_mode: Mode,
    systemlib: bool,
    default_experimental_features: HashSet<FeatureName>,
) -> (Vec<SyntaxError>, bool, HashSet<FeatureName>) {
    <ParserErrors<'a, State>>::parse_errors(
        tree,
        IndexedSourceText::new(tree.text().clone()),
        parser_options,
        hhvm_compat_mode,
        hhi_mode,
        ns_mode,
        systemlib,
        default_experimental_features,
    )
}

pub fn parse_errors_with_text<'a, State: Clone>(
    tree: &'a SyntaxTree<'a, PositionedSyntax<'a>, State>,
    text: IndexedSourceText<'a>,
    parser_options: ParserOptions,
    hhvm_compat_mode: bool,
    hhi_mode: bool,
    ns_mode: Mode,
    systemlib: bool,
    default_experimental_features: HashSet<FeatureName>,
) -> (Vec<SyntaxError>, bool, HashSet<FeatureName>) {
    <ParserErrors<'a, State>>::parse_errors(
        tree,
        text,
        parser_options,
        hhvm_compat_mode,
        hhi_mode,
        ns_mode,
        systemlib,
        default_experimental_features,
    )
}
