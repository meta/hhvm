// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
#![feature(box_patterns)]
mod direct_decl_smart_constructors_generated;

use std::borrow::Cow;
use std::collections::BTreeMap;
use std::rc::Rc;
use std::sync::Arc;

use bstr::BString;
use flatten_smart_constructors::FlattenSmartConstructors;
use hash::HashSet;
use hh_autoimport_rust as hh_autoimport;
use hhbc_string_utils::mangle;
use itertools::Either;
use namespaces::ElaborateKind;
use namespaces_rust as namespaces;
use naming_special_names_rust as naming_special_names;
use oxidized::aast;
use oxidized::ast_defs::Abstraction;
use oxidized::ast_defs::Bop;
use oxidized::ast_defs::ClassishKind;
use oxidized::ast_defs::ConstraintKind;
use oxidized::ast_defs::FunKind;
use oxidized::ast_defs::Id;
use oxidized::ast_defs::ShapeFieldName;
use oxidized::ast_defs::Uop;
use oxidized::ast_defs::Variance;
use oxidized::ast_defs::XhpEnumValue;
use oxidized::decl_parser_options::DeclParserOptions;
use oxidized::direct_decl_parser::Decls;
use oxidized::file_info::Mode;
use oxidized::method_flags::MethodFlags;
use oxidized::namespace_env::Env as NamespaceEnv;
use oxidized::namespace_env::Mode as NamespaceMode;
use oxidized::nast;
use oxidized::pos::Pos;
use oxidized::prop_flags::PropFlags;
use oxidized::s_map::SMap;
use oxidized::s_set::SSet;
use oxidized::shallow_decl_defs;
use oxidized::shallow_decl_defs::Decl;
use oxidized::shallow_decl_defs::DeclConstraintRequirement;
use oxidized::shallow_decl_defs::ShallowClassConst;
use oxidized::shallow_decl_defs::ShallowMethod;
use oxidized::shallow_decl_defs::ShallowProp;
use oxidized::shallow_decl_defs::ShallowTypeconst;
use oxidized::shape_map::ShapeField;
use oxidized::t_shape_map::TShapeField;
use oxidized::t_shape_map::TShapeMap;
use oxidized::typing_defs;
use oxidized::typing_defs::AbstractTypeconst;
use oxidized::typing_defs::Capability::*;
use oxidized::typing_defs::ClassConstKind;
use oxidized::typing_defs::ClassRefinement;
use oxidized::typing_defs::ConcreteTypeconst;
use oxidized::typing_defs::ConstDecl;
use oxidized::typing_defs::ConstValue;
use oxidized::typing_defs::EnumType;
use oxidized::typing_defs::FunElt;
use oxidized::typing_defs::FunImplicitParams;
use oxidized::typing_defs::FunParam;
use oxidized::typing_defs::FunParams;
use oxidized::typing_defs::FunType;
use oxidized::typing_defs::PackageRequirement;
use oxidized::typing_defs::ParamMode;
use oxidized::typing_defs::PosByteString;
use oxidized::typing_defs::PosId;
use oxidized::typing_defs::PosString;
use oxidized::typing_defs::RefinedConst;
use oxidized::typing_defs::RefinedConstBound;
use oxidized::typing_defs::RefinedConstBounds;
use oxidized::typing_defs::ShapeFieldType;
use oxidized::typing_defs::ShapeType;
use oxidized::typing_defs::ShapeTypeSimple;
use oxidized::typing_defs::ShapeTypeSplat;
use oxidized::typing_defs::TaccessType;
use oxidized::typing_defs::Tparam;
use oxidized::typing_defs::TshapeFieldName;
use oxidized::typing_defs::TupleExtra;
use oxidized::typing_defs::TupleType;
use oxidized::typing_defs::Ty;
use oxidized::typing_defs::Ty_;
use oxidized::typing_defs::TypeOrigin;
use oxidized::typing_defs::Typeconst;
use oxidized::typing_defs::TypedefCaseTypeVariant;
use oxidized::typing_defs::TypedefType;
use oxidized::typing_defs::TypedefTypeAssignment;
use oxidized::typing_defs::WhereConstraint;
use oxidized::typing_defs_flags::FunParamFlags;
use oxidized::typing_defs_flags::FunTypeFlags;
use oxidized::typing_reason::Reason;
use oxidized::typing_reason::WitnessDecl;
use oxidized::xhp_attribute;
use parser_core_types::compact_token::CompactToken;
use parser_core_types::indexed_source_text::IndexedSourceText;
use parser_core_types::source_text::SourceText;
use parser_core_types::syntax_kind::SyntaxKind;
use parser_core_types::token_factory::SimpleTokenFactoryImpl;
use parser_core_types::token_kind::TokenKind;
use relative_path::RelativePath;

type SK = SyntaxKind;

#[derive(Clone)]
pub struct DirectDeclSmartConstructors<'o, 't> {
    state: Rc<Impl<'o, 't>>,
    pub token_factory: SimpleTokenFactoryImpl<CompactToken>,
    previous_token_kind: TokenKind,
}

impl<'o, 't> std::ops::Deref for DirectDeclSmartConstructors<'o, 't> {
    type Target = Impl<'o, 't>;
    fn deref(&self) -> &Self::Target {
        &self.state
    }
}

#[derive(Clone)]
pub struct Impl<'o, 't> {
    pub source_text: IndexedSourceText<'t>,
    pub file_attributes: Vec<typing_defs::UserAttribute>,
    pub function_attributes: BTreeMap<String, Vec<typing_defs::UserAttribute>>,

    // const_refs will accumulate all scope-resolution-expressions it
    // encounters while it's "Some"
    const_refs: Option<HashSet<typing_defs::ClassConstRef>>,

    opts: &'o DeclParserOptions,
    filename: Arc<RelativePath>,
    file_mode: oxidized::file_info::Mode,
    namespace_builder: Rc<NamespaceBuilder>,
    classish_name_builder: Option<ClassishNameBuilder>,
    type_parameters: Rc<Vec<SSet>>,
    under_no_auto_dynamic: bool,
    under_no_auto_likes: bool,
    inside_no_auto_dynamic_class: bool,
    pub module: Option<Id>,
    pub package: Option<nast::PackageMembership>,
}

impl<'o, 't> DirectDeclSmartConstructors<'o, 't> {
    pub fn new(
        opts: &'o DeclParserOptions,
        src: &SourceText<'t>,
        file_mode: Mode,
        elaborate_xhp_namespaces_for_facts: bool,
    ) -> Self {
        let source_text = IndexedSourceText::new(src.clone());
        let path = source_text.source_text().file_path();
        let path = path.path_str();
        let package = match opts
            .package_info
            .get_package_for_file(opts.package_support_multifile_tests, path)
        {
            Some(package) => {
                let package: nast::PackageMembership =
                    nast::PackageMembership::PackageConfigAssignment(package.name.1.clone());
                Some(package)
            }
            None => None,
        };
        let filename = source_text.source_text().file_path_rc();
        Self {
            state: Rc::new(Impl {
                source_text,
                filename,
                file_mode,
                file_attributes: Default::default(),
                function_attributes: Default::default(),
                const_refs: None,
                namespace_builder: Rc::new(NamespaceBuilder::new(
                    opts.auto_namespace_map.clone(),
                    opts.disable_xhp_element_mangling,
                    elaborate_xhp_namespaces_for_facts,
                )),
                opts,
                classish_name_builder: None,
                type_parameters: Default::default(),
                under_no_auto_dynamic: false,
                under_no_auto_likes: false,
                inside_no_auto_dynamic_class: false,
                module: None,
                package,
            }),
            token_factory: SimpleTokenFactoryImpl::new(),
            // EndOfFile is used here as a None value (signifying "beginning of
            // file") to save space. There is no legitimate circumstance where
            // we would parse a token and the previous token kind would be
            // EndOfFile.
            previous_token_kind: TokenKind::EndOfFile,
        }
    }

    fn qualified_name_from_parts(&self, parts: &[Node], pos: Pos) -> Id {
        // Count the length of the qualified name, so that we can allocate
        // exactly the right amount of space.
        let mut len = 0;
        for part in parts {
            match part {
                Node::Name(name, _) => len += name.len(),
                Node::Token(t) if t.kind() == TokenKind::Backslash => len += 1,
                Node::ListItem(box Node::Name(name, _), _) => len += name.len() + 1,
                Node::ListItem(box Node::Token(t), _) if t.kind() == TokenKind::Namespace => {
                    len += t.width() + 1;
                }
                _ => {}
            }
        }
        // If there's no internal trivia, then we can just copy the
        // qualified name from the original source text.
        if len == pos.end_offset() - pos.start_offset() {
            let qualified_name = Self::str_from_utf8(self.source_text_at_pos(&pos)).into_owned();
            return Id(pos, qualified_name);
        }
        // Allocate `len` bytes and fill them with the fully qualified name.
        let mut qualified_name = String::with_capacity(len);
        for part in parts {
            match part {
                Node::Name(name, _pos) => qualified_name.push_str(name),
                Node::Token(t) if t.kind() == TokenKind::Backslash => qualified_name.push('\\'),
                Node::ListItem(box Node::Name(name, _), _backslash) => {
                    qualified_name.push_str(name);
                    qualified_name.push('\\');
                }
                Node::ListItem(box Node::Token(t), _backslash)
                    if t.kind() == TokenKind::Namespace =>
                {
                    qualified_name.push_str("namespace\\");
                }
                _ => {}
            }
        }
        debug_assert_eq!(len, qualified_name.len());
        debug_assert_eq!(len, qualified_name.capacity());
        Id(pos, qualified_name)
    }

    fn module_name_string_from_parts(&self, parts: Vec<Node>, pos: Pos) -> String {
        // Count the length of the qualified name, so that we can allocate
        // exactly the right amount of space for it in our arena.
        let mut len = 0;
        for part in &parts {
            match part {
                Node::Name(name, _) => len += name.len(),
                Node::ListItem(box Node::Name(name, _), _dot) => len += name.len() + 1,
                _ => {}
            }
        }
        // If there's no internal trivia, then we can just copy the
        // qualified name from the original source text.
        if len == pos.end_offset() - pos.start_offset() {
            return Self::str_from_utf8(self.source_text_at_pos(&pos)).into_owned();
        }
        // Allocate `len` bytes and fill them with the fully qualified name.
        let mut qualified_name = String::with_capacity(len);
        for part in parts {
            match part {
                Node::Name(name, _pos) => qualified_name.push_str(&name),
                Node::ListItem(box Node::Name(name, _), _) => {
                    qualified_name.push_str(&name);
                    qualified_name.push('.');
                }
                _ => {}
            }
        }
        debug_assert_eq!(len, qualified_name.len());
        debug_assert_eq!(len, qualified_name.capacity());
        qualified_name
    }

    /// If the given node is an identifier, XHP name, or qualified name,
    /// elaborate it in the current namespace and return Some. To be used for
    /// the name of a decl in its definition (e.g., "C" in `class C {}` or "f"
    /// in `function f() {}`).
    fn elaborate_defined_id(&self, name: &Node) -> Option<Id> {
        let id = match name {
            Node::Name(name, pos) => Id(pos.clone(), name.clone()),
            Node::XhpName(name, pos) => Id(pos.clone(), name.clone()),
            Node::QualifiedName(parts, pos) => self.qualified_name_from_parts(parts, pos.clone()),
            // This is always an error; e.g. using a reserved word where a name
            // is expected.
            Node::Token(t) | Node::IgnoredToken(t) => {
                let pos = self.token_pos(*t);
                let text = Self::str_from_utf8(self.source_text_at_pos(&pos));
                Id(pos, text.into_owned())
            }
            _ => return None,
        };
        Some(self.namespace_builder.elaborate_defined_id(id))
    }

    /// If the given node is a name (i.e., an identifier or a qualified name),
    /// return Some. No namespace elaboration is performed.
    fn expect_name(&self, name: &Node) -> Option<Id> {
        // If it's a simple identifier, return it.
        if let id @ Some(_) = name.as_id() {
            return id;
        }
        match name {
            Node::QualifiedName(parts, pos) => {
                Some(self.qualified_name_from_parts(parts, pos.clone()))
            }
            // The IgnoredToken case is always an error; e.g. using a reserved
            // word where a name is expected. The Token case is not an error if
            // the token is TokenKind::XHP (which is legal to use as a name),
            // but an error otherwise (since we expect a Name or QualifiedName
            // here, and the Name case would have been handled in `as_id`
            // above).
            Node::Token(t) | Node::IgnoredToken(t) => {
                let pos = self.token_pos(*t);
                let text = Self::str_from_utf8(self.source_text_at_pos(&pos));
                Some(Id(pos, text.into_owned()))
            }
            _ => None,
        }
    }

    /// Fully qualify the given identifier as a type name (with consideration
    /// to `use` statements in scope).
    fn elaborate_id(&self, Id(pos, name): Id) -> Id {
        Id(pos, self.elaborate_raw_id(&name).into_owned())
    }

    /// Fully qualify the given identifier as a type name (with consideration
    /// to `use` statements in scope).
    fn elaborate_raw_id<'s>(&self, id: &'s str) -> Cow<'s, str> {
        self.namespace_builder
            .elaborate_raw_id(ElaborateKind::Class, id)
    }

    //// Should we preserve this user attribute in the decls?
    fn keep_user_attribute(&self, attr: &UserAttributeNode) -> bool {
        let name = &attr.name.1;
        self.opts.keep_user_attributes
            || name == "__GatedByFeatureFlag"
            || name == "__NoAutoDynamic"
            || name == "__NoAutoLikes"
            || name == "__Overlapping"
            || name == "__Sealed"
            || name == "__TestsBypassVisibility"
    }

    fn fold_string_concat(&self, expr: &nast::Expr, acc: &mut BString) -> bool {
        match expr {
            aast::Expr(
                _,
                _,
                aast::Expr_::ClassConst(box (
                    aast::ClassId(_, _, aast::ClassId_::CI(Id(_, class_name))),
                    _, // required to be "class" in constant initializer
                ))
                | aast::Expr_::Nameof(box aast::ClassId(_, _, aast::ClassId_::CI(Id(_, class_name)))),
            ) => {
                // Imagine the case <<MyFancyEnum('foo'.X::class)>>
                // We would expect a user attribute parameter to concatenate
                // and return a string of 'fooX'.
                // Since `X::class` after elaboration returns the string '\X'
                // we opt to strip the prefix here to successfully concatenate the string
                // into human-readable formats.
                // The Facts parser handles this for AttributeParam::Classname
                // but not AttributeParam::String
                let raw_id = self.elaborate_raw_id(class_name);
                let name = raw_id.as_bytes();
                if name.starts_with(b"\\") {
                    acc.extend_from_slice(&name[1..]);
                } else {
                    acc.extend_from_slice(name);
                }
                true
            }
            aast::Expr(_, _, aast::Expr_::String(val)) => {
                acc.extend_from_slice(val);
                true
            }
            aast::Expr(
                _,
                _,
                aast::Expr_::Binop(box aast::Binop {
                    bop: Bop::Dot,
                    lhs,
                    rhs,
                }),
            ) => self.fold_string_concat(lhs, acc) && self.fold_string_concat(rhs, acc),
            _ => false,
        }
    }

    /// Fully qualify the given identifier as a constant name (with
    /// consideration to `use` statements in scope).
    fn elaborate_const_id(&self, Id(pos, name): Id) -> Id {
        Id(
            pos,
            self.namespace_builder
                .elaborate_raw_id(ElaborateKind::Const, &name)
                .into_owned(),
        )
    }

    fn elaborate_class_id(&self, class_name: &Node) -> Option<Id> {
        self.expect_name(class_name).map(|id| {
            if matches!(class_name, Node::XhpName(..))
                && self.opts.disable_xhp_element_mangling
                && self.opts.keep_user_attributes
            {
                // for facts, allow xhp class consts to be mangled later
                // on even when xhp_element_mangling is disabled
                Id(id.0, format!("\\{}", id.1))
            } else {
                self.elaborate_id(id)
            }
        })
    }

    fn start_accumulating_const_refs(&mut self) {
        let this = Rc::make_mut(&mut self.state);
        this.const_refs = Some(Default::default());
    }

    fn accumulate_const_ref(&mut self, class_id: &aast::ClassId<(), ()>, Id(_, value_name): &Id) {
        let this = Rc::make_mut(&mut self.state);
        // The decl for a class constant stores a list of all the scope-resolution expressions
        // it contains. For example "const C=A::X" stores A::X, and "const D=self::Y" stores self::Y.
        // (This is so we can detect cross-type circularity in constant initializers).
        // TODO: Hack is the wrong place to detect circularity (because we can never do
        // it completely soundly, and because it's a cross-body problem). The right place
        // to do it is in a linter. All this should be removed from here and put into a linter.
        if let Some(const_refs) = &mut this.const_refs {
            match &class_id.2 {
                nast::ClassId_::CIreified(sid) | nast::ClassId_::CI(sid) => {
                    const_refs.insert(typing_defs::ClassConstRef(
                        typing_defs::ClassConstFrom::From(sid.1.clone()),
                        value_name.clone(),
                    ));
                }
                nast::ClassId_::CIself => {
                    const_refs.insert(typing_defs::ClassConstRef(
                        typing_defs::ClassConstFrom::Self_,
                        value_name.clone(),
                    ));
                }
                nast::ClassId_::CIparent | nast::ClassId_::CIstatic | nast::ClassId_::CIexpr(_) => {
                    // Not allowed
                }
            }
        }
    }

    fn stop_accumulating_const_refs(&mut self) -> Vec<typing_defs::ClassConstRef> {
        let this = Rc::make_mut(&mut self.state);
        match this.const_refs.take() {
            Some(const_refs) => {
                let mut elements: Vec<_> = const_refs.into_iter().collect();
                elements.sort_unstable();
                elements
            }
            None => Vec::new(),
        }
    }

    pub fn into_inner(self) -> Impl<'o, 't> {
        match Rc::try_unwrap(self.state) {
            Ok(state) => state,
            Err(_) => panic!("dangling ref"),
        }
    }

    fn make_memberof_type(
        &mut self,
        base_pos: &Pos,
        pos: &Pos,
        enum_class_ty: Ty,
        base_ty: Ty,
        wrap_like: bool,
    ) -> Ty {
        let base_ty = if wrap_like && self.opts.everything_sdt {
            Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(base_pos.clone())),
                Box::new(Ty_::Tlike(base_ty)),
            )
        } else {
            base_ty
        };

        let type_ = Ty_::Tapply(
            (pos.clone(), "\\HH\\MemberOf".into()),
            vec![enum_class_ty, base_ty],
        );

        Ty(
            Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
            Box::new(type_),
        )
    }
}

fn prefix_slash(name: &str) -> String {
    format!("\\{}", name)
}

fn strip_dollar_prefix(name: &str) -> &str {
    name.trim_start_matches('$')
}

const TANY_: Ty_ = Ty_::Tany(oxidized::tany_sentinel::TanySentinel);

fn tany() -> Ty {
    Ty(Reason::NoReason, Box::new(TANY_))
}

/// Create a Tapply for the HH\string type.
fn mk_string_ty_(pos: Pos) -> Ty_ {
    Ty_::Tapply(
        (pos, naming_special_names::typehints::HH_STRING.to_string()),
        vec![],
    )
}

#[derive(Debug)]
struct Modifiers {
    is_static: bool,
    visibility: aast::Visibility,
    is_abstract: bool,
    is_final: bool,
    is_readonly: bool,
}

fn read_member_modifiers<'b>(modifiers: impl Iterator<Item = &'b Node>) -> Modifiers {
    let mut ret = Modifiers {
        is_static: false,
        visibility: aast::Visibility::Public,
        is_abstract: false,
        is_final: false,
        is_readonly: false,
    };
    for modifier in modifiers {
        if let Some(vis) = modifier.as_visibility() {
            match (ret.visibility, vis) {
                (aast::Visibility::Protected, aast::Visibility::Internal) => {
                    ret.visibility = aast::Visibility::ProtectedInternal
                }
                (aast::Visibility::Internal, aast::Visibility::Protected) => {
                    ret.visibility = aast::Visibility::ProtectedInternal
                }
                _ => ret.visibility = vis,
            }
        }
        match modifier.token_kind() {
            Some(TokenKind::Static) => ret.is_static = true,
            Some(TokenKind::Abstract) => ret.is_abstract = true,
            Some(TokenKind::Final) => ret.is_final = true,
            Some(TokenKind::Readonly) => ret.is_readonly = true,
            _ => {}
        }
    }
    ret
}

#[derive(Clone, Debug)]
struct NamespaceBuilder {
    stack: Vec<NamespaceEnv>,
    elaborate_xhp_namespaces_for_facts: bool,
}

impl NamespaceBuilder {
    fn new(
        auto_ns_map: Vec<(String, String)>,
        disable_xhp_element_mangling: bool,
        elaborate_xhp_namespaces_for_facts: bool,
    ) -> Self {
        let mut ns_uses = SMap::new();
        for &alias in hh_autoimport::NAMESPACES {
            ns_uses.insert(alias.to_owned(), format!("HH\\{}", alias));
        }
        for (alias, ns) in auto_ns_map.iter().cloned() {
            ns_uses.insert(alias, ns);
        }

        let mut class_uses = SMap::new();
        for &class in hh_autoimport::TYPES {
            class_uses.insert(class.to_owned(), format!("HH\\{}", class));
        }

        Self {
            stack: vec![NamespaceEnv {
                ns_uses,
                class_uses,
                fun_uses: SMap::new(),
                const_uses: SMap::new(),
                name: None,
                mode: NamespaceMode::ForTypecheck,
                disable_xhp_element_mangling,
            }],
            elaborate_xhp_namespaces_for_facts,
        }
    }

    fn push_namespace<'s>(&mut self, name: Option<Cow<'s, str>>) {
        let current = self.current_namespace();
        let mut nsenv = self.stack.last().unwrap().clone();
        if let Some(name) = name {
            let fully_qualified = match current {
                None => name.into_owned(),
                Some(current) => {
                    let mut fully_qualified =
                        String::with_capacity(current.len() + name.as_ref().len() + 1);
                    fully_qualified.push_str(current);
                    fully_qualified.push('\\');
                    fully_qualified.push_str(name.as_ref());
                    fully_qualified
                }
            };
            nsenv.name = Some(fully_qualified);
        } else {
            nsenv.name = current.map(|s| s.to_owned());
        }
        self.stack.push(nsenv);
    }

    fn pop_namespace(&mut self) {
        // We'll never push a namespace for a declaration of items in the global
        // namespace (e.g., `namespace { ... }`), so only pop if we are in some
        // namespace other than the global one.
        if self.stack.len() > 1 {
            self.stack.pop().unwrap();
        }
    }

    // push_namespace(Y) + pop_namespace() + push_namespace(X) should be equivalent to
    // push_namespace(Y) + push_namespace(X) + pop_previous_namespace()
    fn pop_previous_namespace(&mut self) {
        if self.stack.len() > 2 {
            let last = self.stack.pop().unwrap().name.unwrap_or_default();
            let previous = self.stack.pop().unwrap().name.unwrap_or_default();
            assert!(last.starts_with(&previous));
            let name = &last[previous.len() + 1..last.len()];
            self.push_namespace(Some(Cow::Borrowed(name)));
        }
    }

    fn current_namespace(&self) -> Option<&str> {
        self.stack.last().and_then(|nsenv| nsenv.name.as_deref())
    }

    fn add_import(&mut self, kind: NamespaceUseKind, name: &str, aliased_name: Option<&str>) {
        let stack_top = &mut self
            .stack
            .last_mut()
            .expect("Attempted to get the current import map, but namespace stack was empty");
        let aliased_name = aliased_name.unwrap_or_else(|| {
            name.rsplit_terminator('\\')
                .next()
                .expect("Expected at least one entry in import name")
        });
        let name = name.trim_end_matches('\\');
        let name = if name.starts_with('\\') {
            name.to_owned()
        } else {
            prefix_slash(name)
        };
        match kind {
            NamespaceUseKind::Type => {
                stack_top.class_uses.insert(aliased_name.into(), name);
            }
            NamespaceUseKind::Namespace => {
                stack_top.ns_uses.insert(aliased_name.into(), name);
            }
            NamespaceUseKind::Mixed => {
                stack_top
                    .class_uses
                    .insert(aliased_name.into(), name.clone());
                stack_top.ns_uses.insert(aliased_name.into(), name);
            }
        }
    }

    fn elaborate_raw_id<'s>(&self, kind: ElaborateKind, name: &'s str) -> Cow<'s, str> {
        if name.starts_with('\\') {
            Cow::Borrowed(name)
        } else {
            let env = self.stack.last().unwrap();
            match namespaces::elaborate_raw_id(
                env,
                kind,
                name,
                self.elaborate_xhp_namespaces_for_facts,
            ) {
                Some(s) => Cow::Owned(s),
                None => Cow::Borrowed(name),
            }
        }
    }

    fn elaborate_defined_id(&self, Id(pos, name): Id) -> Id {
        let env = self.stack.last().unwrap();
        let name = if env.disable_xhp_element_mangling && name.contains(':') {
            let xhp_name_opt = namespaces::elaborate_xhp_namespace(&name);
            let name = xhp_name_opt.unwrap_or(name);
            if !name.starts_with('\\') {
                namespaces::elaborate_into_current_ns(env, &name)
            } else if self.elaborate_xhp_namespaces_for_facts {
                // allow :foo:bar to be elaborated into \currentnamespace\foo\bar
                namespaces::elaborate_into_current_ns(env, &name[1..])
            } else {
                name
            }
        } else {
            namespaces::elaborate_into_current_ns(env, &name)
        };
        Id(pos, name)
    }
}

/// We saw a classish keyword token followed by a Name, so we make it
/// available as the name of the containing class declaration.
#[derive(Clone, Debug)]
struct ClassishNameBuilder {
    name: String,
    pos: Pos,
    token_kind: TokenKind,
}

#[derive(Debug, Clone)]
pub struct FunParamDecl {
    attributes: Node,
    visibility: Node,
    kind: ParamMode,
    optional: bool,
    readonly: bool,
    #[allow(dead_code)] // TODO(named_parameters): use
    named: bool,
    hint: Node,
    pos: Pos,
    name: Option<String>,
    variadic: bool,
    splat: bool,
    initializer: Node,
    parameter_end: Node,
}

#[derive(Debug, Clone)]
pub struct FunctionHeader {
    name: Node,
    modifiers: Node,
    type_params: Node,
    param_list: Node,
    capability: Node,
    ret_hint: Node,
    readonly_return: Node,
    where_constraints: Node,
}

#[derive(Debug, Clone)]
pub struct RequireClause {
    require_type: Node,
    name: Node,
}

#[derive(Debug, Clone)]
pub struct RequireClauseConstraint {
    name: Node,
}

#[derive(Debug, Clone)]
pub struct TypeParameterDecl {
    name: Node,
    reified: aast::ReifyKind,
    variance: Variance,
    constraints: Vec<(ConstraintKind, Node)>,
    user_attributes: Vec<UserAttributeNode>,
}

#[derive(Debug)]
pub struct ClosureTypeHint {
    #[allow(dead_code)]
    args: Node,
    #[allow(dead_code)]
    ret_hint: Node,
}

#[derive(Debug, Clone)]
pub struct TupleComponentNode {
    optional: bool,
    pre_ellipsis: bool,
    hint: Node,
    ellipsis: bool,
}

#[derive(Debug, Clone)]
pub struct NamespaceUseClause {
    kind: NamespaceUseKind,
    id: Id,
    as_: Option<String>,
}

#[derive(Copy, Clone, Debug)]
enum NamespaceUseKind {
    Type,
    Namespace,
    Mixed,
}

#[derive(Debug, Clone)]
pub struct ConstructorNode {
    method: ShallowMethod,
    properties: Vec<ShallowProp>,
}

#[derive(Debug, Clone)]
pub struct MethodNode {
    method: ShallowMethod,
    is_static: bool,
}

#[derive(Debug, Clone)]
pub struct PropertyNode {
    decls: Vec<ShallowProp>,
    is_static: bool,
}

#[derive(Debug, Clone)]
pub struct XhpClassAttributeDeclarationNode {
    xhp_attr_enum_values: Vec<(String, Vec<XhpEnumValue>)>,
    xhp_attr_decls: Vec<ShallowProp>,
    xhp_attr_uses_decls: Vec<Node>,
}

#[derive(Debug, Clone)]
pub struct XhpClassAttributeNode {
    name: Id,
    tag: Option<xhp_attribute::Tag>,
    needs_init: bool,
    nullable: bool,
    hint: Node,
}

#[derive(Debug, Clone)]
pub struct ShapeFieldNode {
    name: ShapeField,
    type_: ShapeFieldType,
}

#[derive(Debug, Clone)]
enum AttributeParam {
    Classname(Id),
    EnumClassLabel(String),
    String(Pos, BString),
    Int(String),
}

#[derive(Debug, Clone)]
pub struct UserAttributeNode {
    name: Id,
    params: Vec<AttributeParam>,
    /// This is only used for __Deprecated attribute message and CIPP parameters
    string_literal_param: Option<(Pos, BString)>,
    raw_val: Option<String>,
}

mod fixed_width_token {
    use parser_core_types::token_kind::TokenKind;

    #[derive(Copy, Clone)]
    pub struct FixedWidthToken(u64); // { offset: u56, kind: TokenKind }

    const KIND_BITS: u8 = 8;
    const KIND_MASK: u64 = u8::MAX as u64;
    const MAX_OFFSET: u64 = !(KIND_MASK << (64 - KIND_BITS));

    impl FixedWidthToken {
        pub fn new(kind: TokenKind, offset: usize) -> Self {
            // We don't want to spend bits tracking the width of fixed-width
            // tokens. Since we don't track width, verify that this token kind
            // is in fact a fixed-width kind.
            debug_assert!(kind.fixed_width().is_some());

            let offset: u64 = offset.try_into().unwrap();
            if offset > MAX_OFFSET {
                panic!("FixedWidthToken: offset too large: {}", offset);
            }
            Self(offset << KIND_BITS | kind as u8 as u64)
        }

        pub fn offset(self) -> usize {
            (self.0 >> KIND_BITS).try_into().unwrap()
        }

        pub fn kind(self) -> TokenKind {
            TokenKind::try_from_u8(self.0 as u8).unwrap()
        }

        pub fn width(self) -> usize {
            self.kind().fixed_width().unwrap().get()
        }
    }

    impl std::fmt::Debug for FixedWidthToken {
        fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            fmt.debug_struct("FixedWidthToken")
                .field("kind", &self.kind())
                .field("offset", &self.offset())
                .finish()
        }
    }
}
use fixed_width_token::FixedWidthToken;

#[derive(Copy, Clone, Debug)]
pub enum XhpChildrenKind {
    Empty,
    Other,
}

#[derive(Clone, Debug)]
pub enum Node {
    // Nodes which are not useful in constructing a decl are ignored. We keep
    // track of the SyntaxKind for two reasons.
    //
    // One is that the parser needs to know the SyntaxKind of a parsed node in
    // some circumstances (this information is exposed to the parser via an
    // implementation of `smart_constructors::NodeType`). An adapter called
    // WithKind exists to provide a `NodeType` implementation for arbitrary
    // nodes by pairing each node with a SyntaxKind, but in the direct decl
    // parser, we want to avoid the extra 8 bytes of overhead on each node.
    //
    // The second reason is that debugging is difficult when nodes are silently
    // ignored, and providing at least the SyntaxKind of an ignored node helps
    // in tracking down the reason it was ignored.
    Ignored(SyntaxKind),

    // Missing is an ignored node with an offset
    Missing(usize),

    // For tokens with a fixed width (like `using`), we also keep its offset in
    // the source text, so that we can reference the text of the token if it's
    // (erroneously) used as an identifier (e.g., `function using() {}`).
    IgnoredToken(FixedWidthToken),

    List(Vec<Node>),
    BracketedList(Box<(Pos, Vec<Node>, Pos)>),
    Name(String, Pos),
    XhpName(String, Pos),
    Variable(String, Pos),
    QualifiedName(Vec<Node>, Pos),
    ModuleName(Vec<Node>, Pos),
    StringLiteral(BString, Pos),  // For shape keys and const expressions.
    IntLiteral(String, Pos),      // For const expressions.
    FloatingLiteral(String, Pos), // For const expressions.
    BooleanLiteral(String, Pos),  // For const expressions.
    Ty(Box<Ty>),
    XhpEnumTy(Box<(Option<Pos>, Ty, Vec<XhpEnumValue>)>),
    ListItem(Box<Node>, Box<Node>),

    // For the "X=1" in enums "enum E {X=1}" and enum-classes "enum class C {int X=1}",
    // and also for consts via make_const_declaration
    Const(Box<ShallowClassConst>),

    // Stores (X,1,refs) for "X=1" in top-level "const int X=1" and
    // class-const "public const int X=1".
    ConstInitializer(Box<(Node, Node, Vec<typing_defs::ClassConstRef>)>),

    FunParam(Box<FunParamDecl>),
    Attribute(Box<UserAttributeNode>),
    FunctionHeader(Box<FunctionHeader>),
    Constructor(Box<ConstructorNode>),
    Method(Box<MethodNode>),
    Property(PropertyNode),
    EnumUse(Box<Node>),
    TraitUse(Box<Node>),
    XhpClassAttributeDeclaration(Box<XhpClassAttributeDeclarationNode>),
    XhpClassAttribute(Box<XhpClassAttributeNode>),
    XhpAttributeUse(Box<Node>),
    XhpChildrenDeclaration(XhpChildrenKind),
    TypeConstant(Box<ShallowTypeconst>),
    ContextConstraint(ConstraintKind, Box<Node>),
    RequireClause(Box<RequireClause>),
    RequireClauseConstraint(Box<RequireClauseConstraint>),
    ClassishBody(Vec<Node>),
    TypeParameter(Box<TypeParameterDecl>),
    TypeConstraint(ConstraintKind, Box<Node>),
    ShapeFieldSpecifier(Box<ShapeFieldNode>),
    ShapeSplatSpecifier(Box<Node>),
    NamespaceUseClause(Box<NamespaceUseClause>),
    Expr(Box<nast::Expr>),
    TypeParameters(Vec<Tparam>),
    WhereConstraint(Box<WhereConstraint>),
    RefinedConst(String, Box<RefinedConst>),
    EnumClassLabel(String),
    TupleComponent(Box<TupleComponentNode>),
    CaseTypeVariantWithWhereClause(Box<(Node, Vec<WhereConstraint>)>),

    // Non-ignored, fixed-width tokens (e.g., keywords, operators, braces, etc.).
    Token(FixedWidthToken),

    // A single toplevel symbol declaration and its name. This is currently
    // coupled to direct_decl_parser::Decls and Direct_decl_parser.decls
    Decl(Box<(String, Decl)>),
}

impl smart_constructors::NodeType for Node {
    type Output = Node;

    fn extract(self) -> Self::Output {
        self
    }

    fn is_abstract(&self) -> bool {
        self.is_token(TokenKind::Abstract) || self.is_ignored_token_with_kind(TokenKind::Abstract)
    }
    fn is_name(&self) -> bool {
        matches!(self, Node::Name(..)) || self.is_ignored_token_with_kind(TokenKind::Name)
    }
    fn is_qualified_name(&self) -> bool {
        matches!(self, Node::QualifiedName(..)) || matches!(self, Node::Ignored(SK::QualifiedName))
    }
    fn is_prefix_unary_expression(&self) -> bool {
        matches!(
            self,
            Node::Expr(box aast::Expr(_, _, aast::Expr_::Unop(..)))
        ) || matches!(self, Node::Ignored(SK::PrefixUnaryExpression))
    }
    fn is_scope_resolution_expression(&self) -> bool {
        matches!(
            self,
            Node::Expr(box aast::Expr(_, _, aast::Expr_::ClassConst(..)))
        ) || matches!(self, Node::Ignored(SK::ScopeResolutionExpression))
    }
    fn is_missing(&self) -> bool {
        matches!(self, Node::Ignored(SK::Missing) | Node::Missing(..))
    }
    fn is_variable_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::VariableExpression))
    }
    fn is_subscript_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::SubscriptExpression))
    }
    fn is_member_selection_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::MemberSelectionExpression))
    }
    fn is_object_creation_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::ObjectCreationExpression))
    }
    fn is_safe_member_selection_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::SafeMemberSelectionExpression))
    }
    fn is_function_call_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::FunctionCallExpression))
    }
    fn is_list_expression(&self) -> bool {
        matches!(self, Node::Ignored(SK::ListExpression))
    }
}

impl Node {
    /// Collect the Node::Decls from the root Script node, flattening any nested
    /// namespaces or similar along the way.
    pub fn script_decls(self, ocaml_order: bool) -> Decls {
        let mut decls = Decls::default();
        fn flatten(acc: &mut Decls, node: Node) {
            match node {
                Node::Decl(box (name, decl)) => acc.add(name, decl),
                Node::List(_) => node.into_iter().for_each(|n| flatten(acc, n)),
                _ => {}
            };
        }
        flatten(&mut decls, self);
        if ocaml_order {
            // Direct decl parser returns decls in source order, but to match OCaml
            // we reverse them back.
            decls.rev();
        }
        decls
    }

    fn is_token(&self, kind: TokenKind) -> bool {
        self.token_kind() == Some(kind)
    }

    fn token_kind(&self) -> Option<TokenKind> {
        match self {
            Node::Token(token) => Some(token.kind()),
            _ => None,
        }
    }

    fn is_ignored_token_with_kind(&self, kind: TokenKind) -> bool {
        match self {
            Node::IgnoredToken(token) => token.kind() == kind,
            _ => false,
        }
    }

    fn as_slice(&self) -> &[Self] {
        match self {
            Node::List(items) | Node::BracketedList(box (_, items, _)) => items,
            n if n.is_ignored() => &[],
            n => std::slice::from_ref(n),
        }
    }

    fn into_vec(self) -> Vec<Self> {
        match self {
            Node::List(items) | Node::BracketedList(box (_, items, _)) => items,
            n if n.is_ignored() => vec![],
            n => vec![n],
        }
    }

    fn iter(&self) -> impl DoubleEndedIterator<Item = &Node> {
        self.as_slice().iter()
    }

    fn into_iter(self) -> impl DoubleEndedIterator<Item = Node> {
        self.into_vec().into_iter()
    }

    // The number of elements which would be yielded by `self.iter()`.
    fn len(&self) -> usize {
        self.as_slice().len()
    }

    fn as_visibility(&self) -> Option<aast::Visibility> {
        match self.token_kind() {
            Some(TokenKind::Private) => Some(aast::Visibility::Private),
            Some(TokenKind::Protected) => Some(aast::Visibility::Protected),
            Some(TokenKind::Public) => Some(aast::Visibility::Public),
            Some(TokenKind::Internal) => Some(aast::Visibility::Internal),
            _ => None,
        }
    }

    // If this node is a simple unqualified identifier, return its position and text.
    fn as_id(&self) -> Option<Id> {
        match self {
            Node::Name(name, pos) | Node::XhpName(name, pos) => Some(Id(pos.clone(), name.clone())),
            _ => None,
        }
    }

    // If this node is a Variable token, return its position and text.
    // As an attempt at error recovery (when the dollar sign is omitted), also
    // return other unqualified identifiers (i.e., the Name token kind).
    fn as_variable(&self) -> Option<Id> {
        match self {
            Node::Variable(name, pos) | Node::Name(name, pos) => {
                Some(Id(pos.clone(), name.clone()))
            }
            _ => None,
        }
    }

    fn is_ignored(&self) -> bool {
        matches!(
            self,
            Node::Ignored(..) | Node::IgnoredToken(..) | Node::Missing(..)
        )
    }

    fn is_present(&self) -> bool {
        !self.is_ignored()
    }

    fn contains_marker_attribute(&self, name: &str) -> bool {
        self.iter().any(|node| match node {
            Node::Attribute(box UserAttributeNode {
                name: Id(_pos, attr_name),
                params,
                string_literal_param: None,
                raw_val: None,
            }) if params.is_empty() => attr_name == name,
            _ => false,
        })
    }
}

#[derive(Debug)]
struct Attributes {
    deprecated: Option<String>,
    reifiable: Option<Pos>,
    late_init: bool,
    const_: bool,
    lsb: bool,
    memoize: bool,
    memoizelsb: bool,
    override_: bool,
    enforceable: Option<Pos>,
    accept_disposable: bool,
    ignore_readonly_error: bool,
    dynamically_callable: bool,
    returns_disposable: bool,
    php_std_lib: bool,
    soft: bool,
    support_dynamic_type: bool,
    no_support_dynamic_type: bool,
    no_auto_likes: bool,
    safe_global_variable: bool,
    dynamically_referenced: bool,
    needs_concrete: bool,
    tests_bypass_visibility: bool,
    require_package: Option<PackageRequirement>,
    gated_by_feature_flag: Option<String>,
}

impl<'o, 't> Impl<'o, 't> {
    fn class_decl(&mut self, name: String, decl: shallow_decl_defs::ShallowClass) -> Node {
        self.under_no_auto_dynamic = false;
        self.under_no_auto_likes = false;
        self.inside_no_auto_dynamic_class = false;
        self.classish_name_builder = None;
        Node::Decl(Box::new((name, Decl::Class(decl))))
    }
    fn fun_decl(&mut self, name: String, decl: typing_defs::FunElt) -> Node {
        self.under_no_auto_dynamic = false;
        self.under_no_auto_likes = false;
        Node::Decl(Box::new((name, Decl::Fun(decl))))
    }
    fn typedef_decl(&mut self, name: String, decl: typing_defs::TypedefType) -> Node {
        self.under_no_auto_dynamic = false;
        self.under_no_auto_likes = false;
        Node::Decl(Box::new((name, Decl::Typedef(decl))))
    }
    fn const_decl(&mut self, name: String, decl: typing_defs::ConstDecl) -> Node {
        self.under_no_auto_dynamic = false;
        self.under_no_auto_likes = false;
        Node::Decl(Box::new((name, Decl::Const(decl))))
    }
    fn module_decl(&mut self, name: String, decl: typing_defs::ModuleDefType) -> Node {
        self.under_no_auto_dynamic = false;
        self.under_no_auto_likes = false;
        Node::Decl(Box::new((name, Decl::Module(decl))))
    }

    fn user_attribute_to_decl(&self, attr: UserAttributeNode) -> shallow_decl_defs::UserAttribute {
        use shallow_decl_defs::UserAttributeParam as UAP;
        shallow_decl_defs::UserAttribute {
            name: attr.name.into(),
            params: attr
                .params
                .into_iter()
                .map(|p| match p {
                    AttributeParam::Classname(cls) => UAP::Classname(cls.1),
                    AttributeParam::EnumClassLabel(lbl) => UAP::EnumClassLabel(lbl),
                    AttributeParam::String(_, s) => UAP::String(s),
                    AttributeParam::Int(i) => UAP::Int(i),
                })
                .collect(),
            raw_val: attr.raw_val,
        }
    }

    fn get_current_classish_name(&self) -> Option<(&str, &Pos)> {
        let builder = self.classish_name_builder.as_ref()?;
        Some((&builder.name, &builder.pos))
    }

    fn in_interface(&self) -> bool {
        matches!(
            self.classish_name_builder.as_ref(),
            Some(ClassishNameBuilder {
                token_kind: TokenKind::Interface,
                ..
            })
        )
    }

    fn in_trait(&self) -> bool {
        matches!(
            self.classish_name_builder.as_ref(),
            Some(ClassishNameBuilder {
                token_kind: TokenKind::Trait,
                ..
            })
        )
    }

    fn lexed_name_after_classish_keyword(&mut self, name: String, pos: Pos, token_kind: TokenKind) {
        if self.classish_name_builder.is_none() {
            let name = if name.starts_with(':') {
                prefix_slash(&name)
            } else {
                name
            };
            self.classish_name_builder = Some(ClassishNameBuilder {
                name,
                pos,
                token_kind,
            });
        }
    }
}

impl<'o, 't> DirectDeclSmartConstructors<'o, 't> {
    fn token_bytes(&self, token: &CompactToken) -> &'t [u8] {
        self.source_text
            .source_text()
            .sub(token.start_offset(), token.width())
    }

    // Check that the slice is valid UTF-8. If it is, return a &str referencing
    // the same data.
    fn str_from_utf8<'s>(slice: &'s [u8]) -> Cow<'s, str> {
        String::from_utf8_lossy(slice)
    }

    fn merge(pos1: impl Into<Option<Pos>>, pos2: impl Into<Option<Pos>>) -> Pos {
        match (pos1.into(), pos2.into()) {
            (None, None) => Pos::NONE,
            (Some(pos), None) | (None, Some(pos)) => pos,
            (Some(pos1), Some(pos2)) => match (pos1.is_none(), pos2.is_none()) {
                (true, true) => Pos::NONE,
                (true, false) => pos2,
                (false, true) => pos1,
                (false, false) => Pos::merge_without_checking_filename(&pos1, &pos2),
            },
        }
    }

    fn merge_positions(&self, node1: &Node, node2: &Node) -> Pos {
        Self::merge(self.get_pos_opt(node1), self.get_pos_opt(node2))
    }

    fn pos_from_slice(&self, nodes: &[Node]) -> Pos {
        nodes.iter().fold(Pos::NONE, |acc, node| {
            Self::merge(acc, self.get_pos_opt(node))
        })
    }

    fn get_pos(&self, node: &Node) -> Pos {
        self.get_pos_opt(node).map_or(Pos::NONE, |p| p)
    }

    fn get_pos_opt(&self, node: &Node) -> Option<Pos> {
        let pos = match node {
            Node::Name(_, pos) | Node::Variable(_, pos) => pos.clone(),
            Node::Ty(box ty) => return ty.get_pos().cloned(),
            Node::XhpName(_, pos) => pos.clone(),
            Node::QualifiedName(_, pos) => pos.clone(),
            Node::ModuleName(_, pos) => pos.clone(),
            Node::IntLiteral(_, pos)
            | Node::FloatingLiteral(_, pos)
            | Node::StringLiteral(_, pos)
            | Node::BooleanLiteral(_, pos) => pos.clone(),
            Node::ListItem(fst, snd) => self.merge_positions(fst, snd),
            Node::List(items) => self.pos_from_slice(items),
            Node::BracketedList(box (first_pos, inner_list, second_pos)) => Self::merge(
                first_pos.clone(),
                Self::merge(
                    Some(self.pos_from_slice(inner_list)),
                    Some(second_pos.clone()),
                ),
            ),
            Node::Expr(box aast::Expr(_, pos, _)) => pos.clone(),
            Node::Token(token) => self.token_pos(*token),
            _ => return None,
        };
        if pos.is_none() { None } else { Some(pos) }
    }

    fn token_pos(&self, token: FixedWidthToken) -> Pos {
        let start = token.offset();
        let end = start + token.width();
        let start = self.source_text.offset_to_file_pos_triple(start);
        let end = self.source_text.offset_to_file_pos_triple(end);
        Pos::from_lnum_bol_offset(self.filename.clone(), start, end)
    }

    fn node_to_expr(&self, node: Node) -> Either<nast::Expr, Pos> {
        let pos = self.get_pos(&node);
        let expr_ = match node {
            Node::Expr(box expr) => return Either::Left(expr),
            Node::IntLiteral(s, _) => aast::Expr_::Int(s),
            Node::FloatingLiteral(s, _) => aast::Expr_::Float(s),
            Node::StringLiteral(s, _) => aast::Expr_::String(s),
            Node::BooleanLiteral(s, _) => {
                if s.eq_ignore_ascii_case("true") {
                    aast::Expr_::True
                } else {
                    aast::Expr_::False
                }
            }
            Node::Token(t) if t.kind() == TokenKind::NullLiteral => aast::Expr_::Null,
            Node::Name(..) | Node::QualifiedName(..) => match self.expect_name(&node) {
                None => {
                    return Either::Right(pos);
                }
                Some(n) => aast::Expr_::Id(Box::new(self.elaborate_const_id(n))),
            },
            _ => return Either::Right(pos),
        };
        Either::Left(aast::Expr((), pos, expr_))
    }

    fn node_to_str(&self, node: Node, semicolon: &Node) -> String {
        self.node_to_str_wpos(
            node,
            self.get_pos(semicolon).to_start_and_end_lnum_bol_offset().0,
        )
    }

    fn node_to_str_wpos(&self, node: Node, end: (usize, usize, usize)) -> String {
        let expr = self.node_to_expr(node);
        match expr {
            // Only some nodes have a simple translate to an expression
            // Since some nodes *are* expressions, we prefer to write the
            // to_string logic once for expressions
            Either::Left(expr) => self.expr_to_str(expr),

            // This is usually complex shapes / vec / dict etc. and the
            // actual value is NOT in the nodes so we have to yank the text
            Either::Right(pos) => {
                Self::str_from_utf8(self.source_text_at_pos(&Pos::from_lnum_bol_offset(
                    self.filename.clone(),
                    pos.to_start_and_end_lnum_bol_offset().1,
                    end,
                )))
                .trim()
                .to_string()
            }
        }
    }

    fn expr_to_str(&self, expr: aast::Expr<(), ()>) -> String {
        match expr.2 {
            // Simple literals (99% of the cases)
            aast::Expr_::Int(s) => s,
            aast::Expr_::Float(s) => s,
            aast::Expr_::True => "true".to_string(),
            aast::Expr_::False => "false".to_string(),
            aast::Expr_::Null => "null".to_string(),

            // Default to actual text
            // ... VariableExpression, BinaryExpression, SubscriptExpression,
            //     FunctionCallExpression, ConditionalExpression ...
            _ => Self::str_from_utf8(self.source_text_at_pos(&expr.1)).to_string(),
        }
    }

    /// For a class-constant initializer, returns its recorded value when it is a
    /// recordable kind (int, string, nameof, ::class, or a constant reference),
    /// and None for arithmetic and other unsupported expressions.
    fn const_value(&self, value: &Node) -> Option<ConstValue> {
        match value {
            Node::IntLiteral(s, _) => Self::int_const_value(s, false),
            // Only valid UTF-8 strings are recorded; non-UTF-8 bytes -> None
            // (treated as computed) so distinct byte strings can't collapse to
            // the same recorded value.
            Node::StringLiteral(s, _) => {
                String::from_utf8(s.to_vec()).ok().map(ConstValue::CVString)
            }
            Node::Expr(box aast::Expr(_, _, expr_)) => match expr_ {
                // `nameof C` (a string) and `C::class` (a class pointer) are kept
                // as distinct variants so they never compare equal.
                aast::Expr_::ClassConst(box (
                    aast::ClassId(_, _, aast::ClassId_::CI(Id(_, class_name))),
                    (_, const_name),
                )) if const_name == "class" => {
                    Some(ConstValue::CVClassPointer(mangle(class_name.clone())))
                }
                // `C::A` (accessing another constant): record the accessed class
                // and constant so callers can resolve the reference through decl.
                aast::Expr_::ClassConst(box (
                    aast::ClassId(_, _, aast::ClassId_::CI(Id(_, class_name))),
                    (_, const_name),
                )) => Some(ConstValue::CVConstAccess(
                    class_name.clone(),
                    const_name.clone(),
                )),
                aast::Expr_::Nameof(box aast::ClassId(
                    _,
                    _,
                    aast::ClassId_::CI(Id(_, class_name)),
                )) => Some(ConstValue::CVNameof(mangle(class_name.clone()))),
                aast::Expr_::Nameof(box aast::ClassId(_, _, aast::ClassId_::CIself))
                    if !self.in_trait() =>
                {
                    self.get_current_classish_name()
                        .map(|(class_name, _)| ConstValue::CVNameof(mangle(class_name.to_owned())))
                }
                aast::Expr_::Unop(box (Uop::Uminus, aast::Expr(_, _, aast::Expr_::Int(s)))) => {
                    Self::int_const_value(s, true)
                }
                aast::Expr_::Unop(box (Uop::Uplus, aast::Expr(_, _, aast::Expr_::Int(s)))) => {
                    Self::int_const_value(s, false)
                }
                _ => None,
            },
            _ => None,
        }
    }

    /// The value of an int literal, optionally negated. Literals that fit OCaml's
    /// 63-bit `int` use the compact `CVInt`; the rest of the `i64` range
    /// (including `i64::MIN`) uses the `CVLargeInt` slow path, storing the
    /// canonical decimal so no valid Hack literal is dropped and all spellings
    /// of one value (hex, octal, underscores) compare equal. Uses the same
    /// `parse_int` the lowerer uses. Returns None only when the literal is
    /// genuinely out of `i64` range (Hack rejects it too).
    fn int_const_value(s: &str, negate: bool) -> Option<ConstValue> {
        // OCaml `int` is 63-bit: [-(2^62), 2^62 - 1].
        const OCAML_MAX_INT: i64 = i64::MAX >> 1;
        const OCAML_MIN_INT: i64 = i64::MIN >> 1;
        // Parse the signed text directly (rather than parsing the magnitude and
        // negating) so `i64::MIN` -- whose magnitude 2^63 overflows i64 -- is
        // handled: `parse_int` accumulates negatives via `checked_sub`, reaching
        // `i64::MIN` without ever forming +2^63.
        let text = if negate {
            format!("-{s}")
        } else {
            s.to_owned()
        };
        let n = ocaml_helper::parse_int(&text).ok()?;
        if (OCAML_MIN_INT..=OCAML_MAX_INT).contains(&n) {
            Some(ConstValue::CVInt(n as isize))
        } else {
            // Slow path for a literal outside OCaml's 63-bit `int`. Its decimal
            // form is what an intish string canonicalises to, so a large int and
            // the equivalent intish string coincide (see the intish check).
            Some(ConstValue::CVLargeInt(n.to_string()))
        }
    }

    fn node_to_non_ret_ty(&self, node: Node) -> Option<Ty> {
        self.node_to_ty_(node, false)
    }

    fn node_to_ty(&self, node: Node) -> Option<Ty> {
        self.node_to_ty_(node, true)
    }

    fn make_supportdyn(&self, pos: Pos, ty_: Ty_) -> Ty_ {
        Ty_::Tapply(
            (
                pos.clone(),
                naming_special_names::typehints::HH_SUPPORTDYN.into(),
            ),
            vec![Ty(
                Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                Box::new(ty_),
            )],
        )
    }

    fn implicit_sdt(&self) -> bool {
        self.opts.everything_sdt && !self.under_no_auto_dynamic
    }

    fn no_auto_likes(&self) -> bool {
        self.under_no_auto_likes
    }

    fn node_to_ty_(&self, node: Node, allow_non_ret_ty: bool) -> Option<Ty> {
        match node {
            Node::Ty(box Ty(reason, box Ty_::Tprim(aast::Tprim::Tvoid))) if !allow_non_ret_ty => {
                Some(Ty(reason, Box::new(Ty_::Tprim(aast::Tprim::Tnull))))
            }
            Node::Ty(box Ty(reason, box Ty_::Tprim(aast::Tprim::Tnoreturn)))
                if !allow_non_ret_ty =>
            {
                Some(Ty(reason, Box::new(Ty_::Tunion(vec![]))))
            }
            Node::Ty(box ty) => Some(ty),
            Node::TupleComponent(box TupleComponentNode { hint, .. }) => self.node_to_ty(hint),
            Node::Expr(box expr) => {
                fn expr_to_ty(expr: nast::Expr) -> Option<Ty_> {
                    use aast::Expr_::*;
                    match expr.2 {
                        Null => Some(Ty_::Tprim(aast::Tprim::Tnull)),
                        This => Some(Ty_::Tthis),
                        True | False => Some(Ty_::Tprim(aast::Tprim::Tbool)),
                        Int(_) => Some(Ty_::Tprim(aast::Tprim::Tint)),
                        Float(_) => Some(Ty_::Tprim(aast::Tprim::Tfloat)),
                        String(_) | String2(_) | PrefixedString(_) => Some(mk_string_ty_(expr.1)),
                        Unop(box (_, expr)) => expr_to_ty(expr),
                        Hole(box (expr, _, _, _)) => expr_to_ty(expr),

                        ArrayGet(_) | As(_) | Await(_) | Delay(_) | Binop(_) | Assign(_)
                        | Call(_) | Cast(_) | ClassConst(_) | ClassGet(_) | Clone(_)
                        | Collection(_) | Dollardollar(_) | Efun(_) | Eif(_)
                        | EnumClassLabel(_) | ETSplice(_) | ExpressionTree(_)
                        | FunctionPointer(_) | Id(_) | Import(_) | Is(_) | KeyValCollection(_)
                        | Lfun(_) | List(_) | Lplaceholder(_) | Lvar(_) | MethodCaller(_)
                        | New(_) | ObjGet(_) | Omitted | Pair(_) | Pipe(_) | ReadonlyExpr(_)
                        | Shape(_) | Tuple(_) | Upcast(_) | ValCollection(_) | Xml(_)
                        | Yield(_) | Invalid(_) | Package(_) | Nameof(_) | DestructureShape(_)
                        | DestructureTuple(_) => None,
                    }
                }
                Some(Ty(
                    Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(expr.1.clone())),
                    Box::new(expr_to_ty(expr)?),
                ))
            }
            Node::IntLiteral(_, pos) => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                Box::new(Ty_::Tprim(aast::Tprim::Tint)),
            )),
            Node::FloatingLiteral(_, pos) => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                Box::new(Ty_::Tprim(aast::Tprim::Tfloat)),
            )),
            Node::StringLiteral(_, pos) => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos.clone())),
                Box::new(mk_string_ty_(pos)),
            )),
            Node::BooleanLiteral(_, pos) => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                Box::new(Ty_::Tprim(aast::Tprim::Tbool)),
            )),
            Node::Token(t) if t.kind() == TokenKind::This => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(self.token_pos(t))),
                Box::new(Ty_::Tthis),
            )),
            Node::Token(t) if t.kind() == TokenKind::NullLiteral => {
                let pos = self.token_pos(t);
                Some(Ty(
                    Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
                    Box::new(Ty_::Tprim(aast::Tprim::Tnull)),
                ))
            }
            // In coeffects contexts, we get types like `ctx $f` or `$v::C`.
            // Node::Variable is used for the `$f` and `$v`, so that we don't
            // incorrectly attempt to elaborate them as names.
            Node::Variable(name, pos) => Some(Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(Ty_::Tapply((pos, name), vec![])),
            )),
            node => {
                let Id(pos, name) = self.expect_name(&node)?;
                let reason = Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone()));
                let ty_ = if self.is_type_param_in_scope(&name) {
                    Ty_::Tgeneric(name)
                } else {
                    match name.as_str() {
                        "nothing" => Ty_::Tunion(vec![]),
                        "nonnull" => {
                            if self.implicit_sdt() {
                                self.make_supportdyn(pos, Ty_::Tnonnull)
                            } else {
                                Ty_::Tnonnull
                            }
                        }
                        "dynamic" => Ty_::Tdynamic(None),
                        "_" => Ty_::Twildcard,
                        _ => {
                            let name = self.elaborate_raw_id(&name);
                            Ty_::Tapply((pos, name.into_owned()), vec![])
                        }
                    }
                };
                Some(Ty(reason, Box::new(ty_)))
            }
        }
    }

    fn partition_bounds_into_lower_and_upper(
        &self,
        constraints: Node,
        match_constraint: impl Fn(Node) -> Option<(ConstraintKind, Node)>,
    ) -> (Vec<Ty>, Vec<Ty>) {
        let mut super_ = Vec::new();
        let mut as_ = Vec::new();
        for constraint in constraints.into_iter() {
            if let Some((kind, hint)) = match_constraint(constraint) {
                if let Some(ty) = self.node_to_ty(hint) {
                    match kind {
                        ConstraintKind::ConstraintAs => as_.push(ty),
                        ConstraintKind::ConstraintSuper => super_.push(ty),
                        _ => {}
                    }
                }
            }
        }
        (super_, as_)
    }

    fn partition_type_bounds_into_lower_and_upper(&self, constraints: Node) -> (Vec<Ty>, Vec<Ty>) {
        self.partition_bounds_into_lower_and_upper(constraints, |constraint| {
            if let Node::TypeConstraint(kind, hint) = constraint {
                Some((kind, *hint))
            } else {
                None
            }
        })
    }

    fn partition_ctx_bounds_into_lower_and_upper(&self, constraints: Node) -> (Vec<Ty>, Vec<Ty>) {
        self.partition_bounds_into_lower_and_upper(constraints, |constraint| {
            if let Node::ContextConstraint(kind, hint) = constraint {
                Some((kind, *hint))
            } else {
                None
            }
        })
    }

    fn to_attributes(&self, node: &Node) -> Attributes {
        let mut attributes = Attributes {
            deprecated: None,
            reifiable: None,
            late_init: false,
            const_: false,
            lsb: false,
            memoize: false,
            memoizelsb: false,
            override_: false,
            enforceable: None,
            accept_disposable: false,
            ignore_readonly_error: false,
            dynamically_callable: false,
            returns_disposable: false,
            php_std_lib: false,
            soft: false,
            support_dynamic_type: false,
            no_support_dynamic_type: false,
            no_auto_likes: false,
            safe_global_variable: false,
            dynamically_referenced: false,
            needs_concrete: false,
            tests_bypass_visibility: false,
            require_package: None,
            gated_by_feature_flag: None,
        };

        let nodes = match node {
            Node::List(nodes) | Node::BracketedList(box (_, nodes, _)) => nodes,
            _ => return attributes,
        };
        // Iterate in reverse, to match the behavior of OCaml decl in error conditions.
        for attribute in nodes.iter().rev() {
            if let Node::Attribute(box attribute) = attribute {
                match attribute.name.1.as_str() {
                    "__Deprecated" => {
                        attributes.deprecated = attribute
                            .string_literal_param
                            .as_ref()
                            .map(|(_, x)| Self::str_from_utf8(x).into_owned());
                    }
                    "__Reifiable" => attributes.reifiable = Some(attribute.name.0.clone()),
                    "__LateInit" => {
                        attributes.late_init = true;
                    }
                    "__Const" => {
                        attributes.const_ = true;
                    }
                    "__LSB" => {
                        attributes.lsb = true;
                    }
                    "__Memoize" => {
                        attributes.memoize = true;
                    }
                    "__MemoizeLSB" => {
                        attributes.memoizelsb = true;
                    }
                    "__Override" => {
                        attributes.override_ = true;
                    }
                    "__Enforceable" => {
                        attributes.enforceable = Some(attribute.name.0.clone());
                    }
                    "__AcceptDisposable" => {
                        attributes.accept_disposable = true;
                    }
                    "__IgnoreReadonlyError" => {
                        attributes.ignore_readonly_error = true;
                    }
                    "__DynamicallyCallable" => {
                        attributes.dynamically_callable = true;
                    }
                    "__DynamicallyReferenced" => {
                        attributes.dynamically_referenced = true;
                    }
                    "__ReturnDisposable" => {
                        attributes.returns_disposable = true;
                    }
                    "__PHPStdLib" => {
                        attributes.php_std_lib = true;
                    }
                    "__Soft" => {
                        attributes.soft = true;
                    }
                    "__SupportDynamicType" => {
                        attributes.support_dynamic_type = true;
                    }
                    "__NoSupportDynamicType" => {
                        attributes.no_support_dynamic_type = true;
                    }
                    "__NoAutoLikes" => {
                        attributes.no_auto_likes = true;
                    }
                    "__SafeForGlobalAccessCheck" => {
                        attributes.safe_global_variable = true;
                    }
                    "__NeedsConcrete" => {
                        attributes.needs_concrete = true;
                    }
                    "__TestsBypassVisibility" => {
                        attributes.tests_bypass_visibility = true;
                    }
                    "__RequirePackage" => {
                        attributes.require_package =
                            attribute.string_literal_param.as_ref().map(|(pos, x)| {
                                let pkg_name = Self::str_from_utf8(x).into_owned();
                                return PackageRequirement::RPRequire(
                                    oxidized::typing_defs_core::PosString(
                                        pos.clone().into(),
                                        pkg_name,
                                    ),
                                );
                            });
                    }
                    "__SoftRequirePackage" => {
                        attributes.require_package =
                            attribute.string_literal_param.as_ref().map(|(pos, x)| {
                                let pkg_name = Self::str_from_utf8(x).into_owned();
                                return PackageRequirement::RPSoft(
                                    oxidized::typing_defs_core::PosString(
                                        pos.clone().into(),
                                        pkg_name,
                                    ),
                                );
                            });
                    }
                    "__GatedByFeatureFlag" => {
                        attributes.gated_by_feature_flag = attribute
                            .string_literal_param
                            .as_ref()
                            .map(|(_, x)| Self::str_from_utf8(x).into_owned());
                    }
                    _ => {}
                }
            }
        }

        attributes
    }

    // Limited version of node_to_ty that matches behavior of Decl_utils.infer_const
    fn infer_const(&self, name: Node, node: Node) -> Option<Ty> {
        match node {
            Node::StringLiteral(..)
            | Node::BooleanLiteral(..)
            | Node::IntLiteral(..)
            | Node::FloatingLiteral(..)
            | Node::Expr(box aast::Expr(_, _, aast::Expr_::Unop(box (Uop::Uminus, _))))
            | Node::Expr(box aast::Expr(_, _, aast::Expr_::Unop(box (Uop::Uplus, _))))
            | Node::Expr(box aast::Expr(_, _, aast::Expr_::String(..))) => self.node_to_ty(node),
            Node::Token(t) if t.kind() == TokenKind::NullLiteral => {
                let pos = self.token_pos(t);
                Some(Ty(
                    Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                    Box::new(Ty_::Tprim(aast::Tprim::Tnull)),
                ))
            }
            _ => Some(self.tany_with_pos(self.get_pos(&name))),
        }
    }

    fn pop_type_params(&mut self, node: Node) -> Vec<Tparam> {
        match node {
            Node::TypeParameters(tparams) => {
                let this = Rc::make_mut(&mut self.state);
                Rc::make_mut(&mut this.type_parameters).pop().unwrap();
                tparams
            }
            _ => Default::default(),
        }
    }

    fn ret_from_fun_kind(&self, kind: FunKind, type_: Ty) -> Ty {
        let pos = type_.get_pos().cloned().unwrap_or(Pos::NONE);
        match kind {
            FunKind::FAsyncGenerator => Ty(
                Reason::FromWitnessDecl(WitnessDecl::RetFunKindFromDecl(pos.clone(), kind)),
                Box::new(Ty_::Tapply(
                    (pos, naming_special_names::classes::ASYNC_GENERATOR.into()),
                    vec![type_.clone(), type_.clone(), type_],
                )),
            ),
            FunKind::FGenerator => Ty(
                Reason::FromWitnessDecl(WitnessDecl::RetFunKindFromDecl(pos.clone(), kind)),
                Box::new(Ty_::Tapply(
                    (pos, naming_special_names::classes::GENERATOR.into()),
                    vec![type_.clone(), type_.clone(), type_],
                )),
            ),
            FunKind::FAsync => Ty(
                Reason::FromWitnessDecl(WitnessDecl::RetFunKindFromDecl(pos.clone(), kind)),
                Box::new(Ty_::Tapply(
                    (pos, naming_special_names::classes::AWAITABLE.into()),
                    vec![type_],
                )),
            ),
            _ => type_,
        }
    }

    fn is_type_param_in_scope(&self, name: &str) -> bool {
        self.type_parameters.iter().any(|tps| tps.contains(name))
    }

    fn as_fun_implicit_params(&mut self, capability: Node, default_pos: Pos) -> FunImplicitParams {
        /* Note: do not simplify intersections, keep empty / singleton intersections
         * for coeffect contexts
         */
        let capability = match self.node_to_ty(capability) {
            Some(ty) => CapTy(ty),
            None => CapDefaults(default_pos),
        };
        FunImplicitParams { capability }
    }

    fn make_variadic_type(&self, ellipsis: &Node) -> Ty {
        let pos = self.get_pos(ellipsis);
        match ellipsis.token_kind() {
            // Type of unknown fields is mixed, or supportdyn<mixed> under implicit SD
            Some(TokenKind::DotDotDot) => Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(if self.implicit_sdt() {
                    self.make_supportdyn(pos, Ty_::Tmixed)
                } else {
                    Ty_::Tmixed
                }),
            ),
            // Closed shapes and tuples are expressed using `nothing` (empty union) as the type of unknown fields
            _ => Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
                Box::new(Ty_::Tunion(vec![])),
            ),
        }
    }

    fn function_to_ty(
        &mut self,
        is_method: bool,
        attributes: &Node,
        header: FunctionHeader,
        body: Node,
    ) -> Option<(PosId, Ty, Vec<ShallowProp>)> {
        let id_opt = match (is_method, &header.name) {
            // If the name is missing, we use the left paren here, just to get a
            // position to point to.
            (_, Node::Token(t)) if t.kind() == TokenKind::LeftParen => {
                let pos = self.token_pos(*t);
                Some(Id(pos, "".into()))
            }
            (true, Node::Token(t)) if t.kind() == TokenKind::Construct => {
                let pos = self.token_pos(*t);
                Some(Id(pos, naming_special_names::members::__CONSTRUCT.into()))
            }
            (true, name) => self.expect_name(name),
            (false, name) => self.elaborate_defined_id(name),
        };
        let id = id_opt.unwrap_or_else(|| Id(self.get_pos(&header.name), "".into()));
        let attributes = self.to_attributes(attributes);
        let (params, properties, variadic, named_variadic) =
            self.as_fun_params(attributes.no_auto_likes, header.param_list)?;
        let f_pos = self.get_pos(&header.name);
        let implicit_params = self.as_fun_implicit_params(header.capability, f_pos.clone());

        let header_ret_hint_is_present = header.ret_hint.is_present();
        let type_ = match header.name {
            Node::Token(t) if t.kind() == TokenKind::Construct => {
                let pos = self.token_pos(t);
                Ty(
                    Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
                    Box::new(Ty_::Tprim(aast::Tprim::Tvoid)),
                )
            }
            _ => self
                .node_to_ty(header.ret_hint)
                .unwrap_or_else(|| self.tany_with_pos(f_pos)),
        };
        let async_ = header
            .modifiers
            .iter()
            .any(|n| n.is_token(TokenKind::Async));
        let readonly = header
            .modifiers
            .iter()
            .any(|n| n.is_token(TokenKind::Readonly));

        // XXX could a yield token appear legally in an expression tree wrongly
        // influence the enclosing function decl
        let fun_kind = if body.iter().any(|node| node.is_token(TokenKind::Yield)) {
            if async_ {
                FunKind::FAsyncGenerator
            } else {
                FunKind::FGenerator
            }
        } else if async_ {
            FunKind::FAsync
        } else {
            FunKind::FSync
        };
        let type_ = if !header_ret_hint_is_present {
            self.ret_from_fun_kind(fun_kind, type_)
        } else {
            type_
        };
        // TODO(hrust) Put this in a helper. Possibly do this for all flags.
        let mut flags = match fun_kind {
            FunKind::FSync => FunTypeFlags::empty(),
            FunKind::FAsync => FunTypeFlags::ASYNC,
            FunKind::FGenerator => FunTypeFlags::GENERATOR,
            FunKind::FAsyncGenerator => FunTypeFlags::ASYNC | FunTypeFlags::GENERATOR,
        };

        if attributes.returns_disposable {
            flags |= FunTypeFlags::RETURN_DISPOSABLE;
        }
        if attributes.support_dynamic_type || attributes.dynamically_callable {
            flags |= FunTypeFlags::SUPPORT_DYNAMIC_TYPE;
        }
        if header.readonly_return.is_token(TokenKind::Readonly) {
            flags |= FunTypeFlags::RETURNS_READONLY;
        }
        if attributes.memoize || attributes.memoizelsb {
            flags |= FunTypeFlags::IS_MEMOIZED;
        }
        if readonly {
            flags |= FunTypeFlags::READONLY_THIS
        }
        if variadic {
            flags |= FunTypeFlags::VARIADIC
        }
        if named_variadic {
            flags |= FunTypeFlags::NAMED_VARIADIC
        }

        // Pop the type params stack only after creating all inner types.
        let tparams = self.pop_type_params(header.type_params);

        let where_constraints = header
            .where_constraints
            .into_iter()
            .filter_map(|x| match x {
                Node::WhereConstraint(box x) => Some(x),
                _ => None,
            })
            .collect();

        let (params, tparams, implicit_params, where_constraints) =
            self.rewrite_effect_polymorphism(params, tparams, implicit_params, where_constraints);

        let ft = FunType {
            tparams,
            where_constraints,
            params,
            implicit_params,
            ret: type_,
            flags,
            instantiated: true,
        };

        let ty = Ty(
            Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(id.0.clone())),
            Box::new(Ty_::Tfun(ft)),
        );
        Some((id.into(), ty, properties))
    }

    fn as_fun_params(
        &self,
        no_auto_likes: bool,
        list: Node,
    ) -> Option<(FunParams, Vec<ShallowProp>, bool, bool)> {
        match list {
            Node::List(nodes) => {
                let mut params = Vec::with_capacity(nodes.len());
                let mut properties = Vec::new();
                let mut ft_variadic = false;
                let mut ft_named_variadic = false;
                for node in nodes.into_iter() {
                    match node {
                        Node::FunParam(box FunParamDecl {
                            attributes,
                            visibility,
                            kind,
                            optional,
                            readonly,
                            named,
                            hint,
                            pos,
                            name,
                            variadic,
                            splat,
                            initializer,
                            parameter_end,
                        }) => {
                            let attributes = self.to_attributes(&attributes);

                            let type_ = self
                                .node_to_ty(hint)
                                .unwrap_or_else(|| self.tany_with_pos(pos.clone()));
                            // A promoted variadic parameter t... gives rise to a property of type vec<t>
                            let prop_type_ = if variadic {
                                let ty_ = Ty_::Tapply(
                                    (
                                        pos.clone(),
                                        naming_special_names::collections::VEC.to_string(),
                                    ),
                                    vec![type_.clone()],
                                );
                                Ty(
                                    Reason::FromWitnessDecl(WitnessDecl::VarParamFromDecl(
                                        pos.clone(),
                                    )),
                                    Box::new(ty_),
                                )
                            } else if splat {
                                Ty(
                                    Reason::FromWitnessDecl(WitnessDecl::TupleFromSplat(
                                        pos.clone(),
                                    )),
                                    type_.clone().1,
                                )
                            } else {
                                type_.clone()
                            };
                            if let Some(visibility) = visibility.as_visibility() {
                                let name = name.as_deref().unwrap_or("");
                                let name = strip_dollar_prefix(name);
                                let mut flags = PropFlags::empty();
                                flags.set(PropFlags::CONST, attributes.const_);
                                flags.set(PropFlags::NEEDS_INIT, self.file_mode != Mode::Mhhi);
                                flags.set(PropFlags::PHP_STD_LIB, attributes.php_std_lib);
                                flags.set(PropFlags::READONLY, readonly);
                                // Promoted property either marked <<__NoAutoLikes>> on the parameter or on the constructor
                                flags.set(
                                    PropFlags::NO_AUTO_LIKES,
                                    attributes.no_auto_likes || no_auto_likes,
                                );
                                flags.set(
                                    PropFlags::TESTS_BYPASS_VISIBILITY,
                                    attributes.tests_bypass_visibility,
                                );
                                properties.push(ShallowProp {
                                    xhp_attr: None,
                                    name: (pos.clone(), name.to_string()),
                                    type_: prop_type_,
                                    visibility,
                                    flags,
                                });
                            }

                            // These are illegal here--they can only be used on
                            // parameters in a function type hint (see
                            // make_closure_type_specifier and unwrap_mutability).
                            // Unwrap them here anyway for better error recovery.
                            let type_ = match type_ {
                                Ty(_, box Ty_::Tapply((_, sym), mut tys))
                                    if tys.len() == 1
                                        && matches!(
                                            sym.as_str(),
                                            "\\Mutable" | "\\OwnedMutable" | "\\MaybeMutable"
                                        ) =>
                                {
                                    tys.pop().unwrap()
                                }
                                _ => type_,
                            };
                            let mut flags = FunParamFlags::empty();
                            if attributes.accept_disposable {
                                flags |= FunParamFlags::ACCEPT_DISPOSABLE
                            }
                            if attributes.ignore_readonly_error {
                                flags |= FunParamFlags::IGNORE_READONLY_ERROR
                            }
                            if readonly {
                                flags |= FunParamFlags::READONLY
                            }
                            if splat {
                                flags |= FunParamFlags::SPLAT
                            }
                            if named {
                                flags |= FunParamFlags::NAMED
                            }
                            match kind {
                                ParamMode::FPinout => {
                                    flags |= FunParamFlags::INOUT;
                                }
                                ParamMode::FPnormal => {}
                            };

                            if optional || initializer.is_present() {
                                flags |= FunParamFlags::IS_OPTIONAL;
                            }
                            if variadic {
                                if named {
                                    ft_named_variadic = true;
                                } else {
                                    ft_variadic = true;
                                }
                            }
                            let variadic = initializer.is_ignored() && variadic;
                            let type_ = if variadic {
                                Ty(
                                    if name.is_some() {
                                        Reason::FromWitnessDecl(WitnessDecl::VarParamFromDecl(
                                            pos.clone(),
                                        ))
                                    } else {
                                        Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(
                                            pos.clone(),
                                        ))
                                    },
                                    type_.1,
                                )
                            } else if splat {
                                Ty(
                                    Reason::FromWitnessDecl(WitnessDecl::TupleFromSplat(
                                        pos.clone(),
                                    )),
                                    type_.1,
                                )
                            } else {
                                type_
                            };
                            let param = FunParam {
                                pos,
                                name,
                                type_,
                                flags,
                                def_value: if self.opts.include_assignment_values
                                    && initializer.is_present()
                                {
                                    let end = match parameter_end {
                                        Node::Missing(offset) => {
                                            self.source_text.offset_to_file_pos_triple(offset)
                                        }
                                        _ => {
                                            self.get_pos(&initializer)
                                                .to_start_and_end_lnum_bol_offset()
                                                .1
                                        }
                                    };
                                    Some(self.node_to_str_wpos(initializer, end))
                                } else {
                                    None
                                },
                            };
                            params.push(param);
                        }
                        _ => {}
                    }
                }
                Some((params, properties, ft_variadic, ft_named_variadic))
            }
            n if n.is_ignored() => Some((vec![], vec![], false, false)),
            _ => None,
        }
    }

    fn make_shape_field_name(&self, name: Node) -> Option<ShapeFieldName> {
        Some(match name {
            Node::StringLiteral(s, pos) => ShapeFieldName::SFlitStr((pos, s)),
            // TODO: OCaml decl produces SFlitStr here instead of SFregexGroup, so
            // we must also. Looks like int literal keys have become a parse
            // error--perhaps that's why.
            Node::IntLiteral(s, pos) => ShapeFieldName::SFlitStr((pos, s.into())),
            Node::Expr(box aast::Expr(_, _, expr_)) => match expr_ {
                aast::Expr_::ClassConst(box (
                    aast::ClassId(_, _, aast::ClassId_::CI(class_name)),
                    const_name,
                )) => ShapeFieldName::SFclassConst(class_name, const_name),
                aast::Expr_::Nameof(box aast::ClassId(_, _, aast::ClassId_::CI(class_name))) => {
                    ShapeFieldName::SFclassname(class_name)
                }
                // TODO(T199272576) I believe these two cases should be dead by parse error
                aast::Expr_::ClassConst(box (
                    aast::ClassId(_, self_pos, aast::ClassId_::CIself),
                    const_name,
                )) => {
                    let (classish_name, _) = self.get_current_classish_name()?;
                    ShapeFieldName::SFclassConst(
                        Id(self_pos, classish_name.to_string()),
                        const_name,
                    )
                }
                aast::Expr_::Nameof(box aast::ClassId(_, self_pos, aast::ClassId_::CIself)) => {
                    let (classish_name, _) = self.get_current_classish_name()?;
                    ShapeFieldName::SFclassname(Id(self_pos, classish_name.to_string()))
                }
                _ => return None,
            },
            _ => return None,
        })
    }

    fn make_t_shape_field_name(&mut self, ShapeField(field): ShapeField) -> TShapeField {
        TShapeField(match field {
            ShapeFieldName::SFlitStr((pos, x)) => TshapeFieldName::TSFlitStr(PosByteString(pos, x)),
            ShapeFieldName::SFclassname(id) => {
                let pos = id.0.clone();
                TshapeFieldName::TSFclassConst(id.into(), PosString(pos, "class".to_string()))
            }
            ShapeFieldName::SFclassConst(id, (pos, x)) => {
                TshapeFieldName::TSFclassConst(id.into(), PosString(pos, x))
            }
        })
    }

    fn make_apply(&self, base_ty: PosId, type_arguments: Node, pos_to_merge: Pos) -> Node {
        let mut type_arguments: Vec<Ty> = type_arguments
            .into_iter()
            .filter_map(|node| self.node_to_ty(node))
            .collect();

        let pos = Self::merge(base_ty.0.clone(), pos_to_merge);

        // OCaml decl creates a capability with a hint pointing to the entire
        // type (i.e., pointing to `Rx<(function(): void)>` rather than just
        // `(function(): void)`), so we extend the hint position similarly here.
        let extend_capability_pos = |implicit_params: FunImplicitParams| {
            let capability = match implicit_params.capability {
                CapTy(ty) => {
                    let ty = Ty(
                        Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                        ty.1,
                    );
                    CapTy(ty)
                }
                CapDefaults(_) => CapDefaults(pos.clone()),
            };
            FunImplicitParams {
                capability,
                ..implicit_params
            }
        };

        let ty_ = match (&base_ty, &type_arguments[..]) {
            ((_, name), [Ty(_, box Ty_::Tfun(_))]) if name == "\\Pure" => {
                if let Ty(_, box Ty_::Tfun(f)) = type_arguments.pop().unwrap() {
                    Ty_::Tfun(FunType {
                        implicit_params: extend_capability_pos(f.implicit_params),
                        ..f
                    })
                } else {
                    unreachable!()
                }
            }
            _ => Ty_::Tapply(base_ty, type_arguments),
        };

        self.hint_ty(pos, ty_)
    }

    fn hint_ty(&self, pos: Pos, ty_: Ty_) -> Node {
        Node::Ty(Box::new(Ty(
            Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
            Box::new(ty_),
        )))
    }

    fn prim_ty(&self, tprim: aast::Tprim, pos: Pos) -> Node {
        self.hint_ty(pos, Ty_::Tprim(tprim))
    }

    fn tany_with_pos(&self, pos: Pos) -> Ty {
        Ty(
            Reason::FromWitnessDecl(WitnessDecl::WitnessFromDecl(pos)),
            Box::new(TANY_),
        )
    }

    /// The type used when a `vec_or_dict` typehint is missing its key type argument.
    fn vec_or_dict_key(&self, pos: Pos) -> Ty {
        Ty(
            Reason::FromWitnessDecl(WitnessDecl::VecOrDictKey(pos)),
            Box::new(Ty_::Tprim(aast::Tprim::Tarraykey)),
        )
    }

    fn source_text_at_pos(&self, pos: &Pos) -> &'t [u8] {
        let start = pos.start_offset();
        let end = pos.end_offset();
        self.source_text.source_text().sub(start, end - start)
    }

    // While we usually can tell whether to allocate a Tapply or Tgeneric based
    // on our type_parameters stack, *constraints* on type parameters may
    // reference type parameters which we have not parsed yet. When constructing
    // a type parameter list, we use this function to rewrite the type of each
    // constraint, considering the full list of type parameters to be in scope.
    fn convert_tapply_to_tgeneric(&self, ty: Ty) -> Ty {
        let ty_ = match *ty.1 {
            Ty_::Tapply(id, targs) => {
                let converted_targs = targs
                    .into_iter()
                    .map(|targ| self.convert_tapply_to_tgeneric(targ))
                    .collect();
                match self.tapply_should_be_tgeneric(&ty.0, &id) {
                    Some(name) => Ty_::Tgeneric(name),
                    None => Ty_::Tapply(id, converted_targs),
                }
            }
            Ty_::Tlike(ty) => Ty_::Tlike(self.convert_tapply_to_tgeneric(ty)),
            Ty_::Toption(ty) => Ty_::Toption(self.convert_tapply_to_tgeneric(ty)),
            Ty_::TclassPtr(ty) => Ty_::TclassPtr(self.convert_tapply_to_tgeneric(ty)),
            Ty_::Tfun(fun_type) => {
                let convert_param = |param: FunParam| FunParam {
                    type_: self.convert_tapply_to_tgeneric(param.type_),
                    ..param
                };
                let params = fun_type.params.into_iter().map(convert_param).collect();
                let implicit_params = fun_type.implicit_params;
                let ret = self.convert_tapply_to_tgeneric(fun_type.ret);
                Ty_::Tfun(FunType {
                    params,
                    implicit_params,
                    ret,
                    ..fun_type
                })
            }
            Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                origin: _,
                unknown_value: kind,
                fields,
            })) => {
                let mut converted_fields = TShapeMap::new();
                for (name, ty) in fields.into_iter() {
                    converted_fields.insert(
                        name,
                        ShapeFieldType {
                            optional: ty.optional,
                            ty: self.convert_tapply_to_tgeneric(ty.ty),
                        },
                    );
                }
                let origin = TypeOrigin::MissingOrigin;
                Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                    origin,
                    unknown_value: kind,
                    fields: converted_fields,
                }))
            }
            Ty_::Tshape(ShapeType::ShapeSplat(ShapeTypeSplat { ss_elems })) => {
                let ss_elems = ss_elems
                    .into_iter()
                    .map(|ty| self.convert_tapply_to_tgeneric(ty))
                    .collect();
                Ty_::Tshape(ShapeType::ShapeSplat(ShapeTypeSplat { ss_elems }))
            }
            Ty_::TvecOrDict(tk, tv) => Ty_::TvecOrDict(
                self.convert_tapply_to_tgeneric(tk),
                self.convert_tapply_to_tgeneric(tv),
            ),
            Ty_::Ttuple(TupleType {
                required,
                optional,
                extra,
            }) => {
                let extra = match extra {
                    TupleExtra::Tvariadic(variadic) => {
                        TupleExtra::Tvariadic(self.convert_tapply_to_tgeneric(variadic))
                    }
                    TupleExtra::Tsplat(hint) => {
                        TupleExtra::Tsplat(self.convert_tapply_to_tgeneric(hint))
                    }
                };
                Ty_::Ttuple(TupleType {
                    required: required
                        .into_iter()
                        .map(|targ| self.convert_tapply_to_tgeneric(targ))
                        .collect(),
                    optional: optional
                        .into_iter()
                        .map(|targ| self.convert_tapply_to_tgeneric(targ))
                        .collect(),
                    extra,
                })
            }
            Ty_::Tintersection(tys) => Ty_::Tintersection(
                tys.into_iter()
                    .map(|ty| self.convert_tapply_to_tgeneric(ty))
                    .collect(),
            ),
            Ty_::Tunion(tys) => Ty_::Tunion(
                tys.into_iter()
                    .map(|ty| self.convert_tapply_to_tgeneric(ty))
                    .collect(),
            ),
            Ty_::Trefinement(root_ty, class_ref) => {
                let convert_refined_const = |rc: RefinedConst| {
                    let RefinedConst { bound, is_ctx } = rc;
                    let bound = match bound {
                        RefinedConstBound::TRexact(ty) => {
                            RefinedConstBound::TRexact(self.convert_tapply_to_tgeneric(ty))
                        }
                        RefinedConstBound::TRloose(bnds) => {
                            let convert_tys = |tys: Vec<Ty>| {
                                tys.into_iter()
                                    .map(|ty| self.convert_tapply_to_tgeneric(ty))
                                    .collect()
                            };
                            RefinedConstBound::TRloose(RefinedConstBounds {
                                lower: convert_tys(bnds.lower),
                                upper: convert_tys(bnds.upper),
                            })
                        }
                    };
                    RefinedConst { bound, is_ctx }
                };
                Ty_::Trefinement(
                    self.convert_tapply_to_tgeneric(root_ty),
                    ClassRefinement {
                        cr_consts: class_ref
                            .cr_consts
                            .into_iter()
                            .map(|(id, ctr)| (id, convert_refined_const(ctr)))
                            .collect(),
                    },
                )
            }
            Ty_::Taccess(_)
            | Ty_::Tany(_)
            | Ty_::Tdynamic(_)
            | Ty_::Tgeneric(_)
            | Ty_::Tmixed
            | Ty_::Twildcard
            | Ty_::Tnonnull
            | Ty_::Tprim(_)
            | Ty_::Tthis => return ty,
        };
        Ty(ty.0, Box::new(ty_))
    }

    // This is the logic for determining if convert_tapply_to_tgeneric should turn
    // a Tapply into a Tgeneric
    fn tapply_should_be_tgeneric(&self, reason: &Reason, id: &PosId) -> Option<String> {
        match reason.pos() {
            // If the name contained a namespace delimiter in the original
            // source text, then it can't have referred to a type parameter
            // (since type parameters cannot be namespaced).
            Some(pos) => {
                if self.source_text_at_pos(pos).contains(&b'\\') {
                    return None;
                }
            }
            None => return None,
        }
        // However, the direct decl parser will unconditionally prefix
        // the name with the current namespace (as it does for any
        // Tapply). We need to remove it.
        match id.1.rsplit('\\').next() {
            Some(name) if self.is_type_param_in_scope(name) => Some(name.to_owned()),
            _ => None,
        }
    }

    fn rewrite_taccess_reasons(ty: Ty, r: Reason) -> Ty {
        // XXX rewrite in place: &mut *ty.1
        let ty_ = match *ty.1 {
            Ty_::Taccess(TaccessType(ty, id)) => Ty_::Taccess(TaccessType(
                Self::rewrite_taccess_reasons(ty, r.clone()),
                id,
            )),
            ty_ => ty_,
        };
        Ty(r, Box::new(ty_))
    }

    fn namespace_use_kind(use_kind: &Node) -> Option<NamespaceUseKind> {
        match use_kind.token_kind() {
            Some(TokenKind::Const) => None,
            Some(TokenKind::Function) => None,
            Some(TokenKind::Type) => Some(NamespaceUseKind::Type),
            Some(TokenKind::Namespace) => Some(NamespaceUseKind::Namespace),
            _ if !use_kind.is_present() => Some(NamespaceUseKind::Mixed),
            _ => None,
        }
    }

    fn has_polymorphic_context(contexts: &[Ty]) -> bool {
        contexts.iter().any(|ty| match &*ty.1 {
            // Hfun_context in the AST
            Ty_::Tapply(root, tys)
            | Ty_::Taccess(TaccessType(Ty(_, box Ty_::Tapply(root, tys)), _))
                if tys.is_empty() =>
            {
                root.1.contains('$')
            }
            Ty_::Taccess(TaccessType(t, _)) => Self::taccess_root_is_generic(t),
            _ => false,
        })
    }

    fn ctx_generic_for_fun(&self, name: &str) -> String {
        format!("T/[ctx {name}]")
    }

    fn ctx_generic_for_dependent(&self, name: &str, cst: &str) -> String {
        format!("T/[{name}::{cst}]")
    }

    // Note: the reason for the divergence between this and the lowerer is that
    // hint Haccess is a flat list, whereas decl ty Taccess is a tree.
    fn taccess_root_is_generic(ty: &Ty) -> bool {
        match ty {
            Ty(_, box Ty_::Tgeneric(_)) => true,
            Ty(_, box Ty_::Taccess(TaccessType(t, _))) => Self::taccess_root_is_generic(t),
            _ => false,
        }
    }

    fn ctx_generic_for_generic_taccess_inner(ty: &Ty, cst: &str) -> String {
        let left = match ty {
            Ty(_, box Ty_::Tgeneric(name)) => name.to_string(),
            Ty(_, box Ty_::Taccess(TaccessType(ty, cst))) => {
                Self::ctx_generic_for_generic_taccess_inner(ty, &cst.1)
            }
            _ => panic!("Unexpected element in Taccess"),
        };
        format!("{}::{}", left, cst)
    }

    fn ctx_generic_for_generic_taccess(&self, ty: &Ty, cst: &str) -> String {
        format!(
            "T/[{}]",
            Self::ctx_generic_for_generic_taccess_inner(ty, cst)
        )
    }

    fn possibly_make_supportdyn_method(
        &mut self,
        class_attributes: &Attributes,
        method: ShallowMethod,
    ) -> ShallowMethod {
        if ((self.implicit_sdt() && !class_attributes.no_support_dynamic_type)
            || class_attributes.support_dynamic_type
            || class_attributes.dynamically_referenced)
            && !method.flags.contains(MethodFlags::SUPPORT_DYNAMIC_TYPE)
        {
            let type_ = match *method.type_.1 {
                Ty_::Tfun(ft) => {
                    let flags = ft.flags | FunTypeFlags::SUPPORT_DYNAMIC_TYPE;
                    let ft = FunType { flags, ..ft };
                    Ty(method.type_.0, Box::new(Ty_::Tfun(ft)))
                }
                _ => method.type_,
            };
            let flags = method.flags | MethodFlags::SUPPORT_DYNAMIC_TYPE;
            ShallowMethod {
                type_,
                flags,
                ..method
            }
        } else {
            method
        }
    }

    // For a polymorphic context with form `ctx $f` (represented here as
    // `Tapply "$f"`), add a type parameter named `Tctx$f`, and rewrite the
    // parameter `(function (ts)[_]: t) $f` as `(function (ts)[Tctx$f]: t) $f`
    fn rewrite_fun_ctx(&self, tparams: &mut Vec<Tparam>, ty: Ty, param_name: &str) -> Ty {
        match *ty.1 {
            Ty_::Tfun(ft) => {
                let cap_ty = match &ft.implicit_params.capability {
                    CapTy(Ty(_, box Ty_::Tintersection(tys))) if tys.len() == 1 => tys[0].clone(),
                    CapTy(ty) => ty.clone(),
                    _ => return Ty(ty.0, Box::new(Ty_::Tfun(ft))), // XXX sucks to rebox
                };
                let pos = match *cap_ty.1 {
                    Ty_::Tapply((pos, sym), _) if &sym == "_" => pos,
                    _ => return Ty(ty.0, Box::new(Ty_::Tfun(ft))), // XXX sucks to rebox
                };
                let name = self.ctx_generic_for_fun(param_name);
                let tparam = Tparam {
                    variance: Variance::Invariant,
                    name: (pos, name.clone()),
                    constraints: vec![],
                    reified: aast::ReifyKind::Erased,
                    user_attributes: vec![],
                };
                tparams.push(tparam);
                let cap_ty = Ty(cap_ty.0, Box::new(Ty_::Tgeneric(name)));
                let ft = FunType {
                    implicit_params: FunImplicitParams {
                        capability: CapTy(cap_ty),
                    },
                    ..ft
                };
                Ty(ty.0, Box::new(Ty_::Tfun(ft)))
            }
            Ty_::Tlike(t) => Ty(
                ty.0,
                Box::new(Ty_::Tlike(self.rewrite_fun_ctx(tparams, t, param_name))),
            ),
            Ty_::Toption(t) => Ty(
                ty.0,
                Box::new(Ty_::Toption(self.rewrite_fun_ctx(tparams, t, param_name))),
            ),
            Ty_::Tapply((p, name), targs)
                if name == naming_special_names::typehints::HH_SUPPORTDYN =>
            {
                if let Some(t) = targs.first() {
                    Ty(
                        ty.0,
                        Box::new(Ty_::Tapply(
                            (p, name),
                            vec![self.rewrite_fun_ctx(tparams, t.clone(), param_name)],
                        )),
                    )
                } else {
                    Ty(ty.0, Box::new(Ty_::Tapply((p, name), targs))) // XXX rebox sucks
                }
            }
            _ => ty,
        }
    }

    fn rewrite_effect_polymorphism(
        &self,
        params: Vec<FunParam>,
        tparams: Vec<Tparam>,
        implicit_params: FunImplicitParams,
        where_constraints: Vec<WhereConstraint>,
    ) -> (
        Vec<FunParam>,
        Vec<Tparam>,
        FunImplicitParams,
        Vec<WhereConstraint>,
    ) {
        let (cap_reason, mut context_tys) = match &implicit_params.capability {
            CapTy(Ty(r, box Ty_::Tintersection(tys))) if Self::has_polymorphic_context(tys) => {
                (r.clone(), tys.clone())
            }
            CapTy(ty) if Self::has_polymorphic_context(std::slice::from_ref(ty)) => {
                (ty.0.clone(), vec![ty.clone()])
            }
            _ => return (params, tparams, implicit_params, where_constraints),
        };
        let tp = |name, constraints| Tparam {
            variance: Variance::Invariant,
            name,
            constraints,
            reified: aast::ReifyKind::Erased,
            user_attributes: Vec::new(),
        };

        // For a polymorphic context with form `$g::C`, if we have a function
        // parameter `$g` with type `G` (where `G` is not a type parameter),
        //   - add a type parameter constrained by $g's type: `T/$g as G`
        //   - replace $g's type hint (`G`) with the new type parameter `T/$g`
        // Then, for each polymorphic context with form `$g::C`,
        //   - add a type parameter `T/[$g::C]`
        //   - add a where constraint `T/[$g::C] = T$g :: C`
        let rewrite_arg_ctx = |tparams: &mut Vec<Tparam>,
                               where_constraints: &mut Vec<WhereConstraint>,
                               ty: Ty,
                               param_pos: Pos,
                               name: &str,
                               context_reason: Reason,
                               cst: PosId|
         -> Ty {
            let rewritten_ty = match &*ty.1 {
                // If the type hint for this function parameter is a type
                // parameter introduced in this function declaration, don't add
                // a new type parameter.
                Ty_::Tgeneric(type_name) if tparams.iter().any(|tp| &tp.name.1 == type_name) => ty,
                // Otherwise, if the parameter is `G $g`, create tparam
                // `T$g as G` and replace $g's type hint
                _ => {
                    let id = (param_pos.clone(), format!("T/{}", name));
                    tparams.push(tp(id.clone(), vec![(ConstraintKind::ConstraintAs, ty)]));
                    Ty(
                        Reason::FromWitnessDecl(WitnessDecl::Hint(param_pos)),
                        Box::new(Ty_::Tgeneric(id.1)),
                    )
                }
            };
            let left_name = self.ctx_generic_for_dependent(name, &cst.1);
            let ty = Ty(context_reason.clone(), rewritten_ty.1.clone());
            let right = Ty(
                context_reason.clone(),
                Box::new(Ty_::Taccess(TaccessType(ty, cst))),
            );
            let left_id = (
                context_reason.pos().cloned().unwrap_or(Pos::NONE),
                left_name,
            );
            let left_ty_ = Ty_::Tgeneric(left_id.1.clone());
            tparams.push(tp(left_id, vec![]));
            let left = Ty(context_reason, Box::new(left_ty_));
            where_constraints.push(WhereConstraint(left, ConstraintKind::ConstraintEq, right));
            rewritten_ty
        };

        let mut tparams = tparams;
        let mut where_constraints = where_constraints;

        let mut ty_by_param: BTreeMap<String, (Ty, Pos)> = params
            .iter()
            .filter_map(|param| {
                Some((
                    param.name.as_ref()?.clone(),
                    (param.type_.clone(), param.pos.clone()),
                ))
            })
            .collect();

        for context_ty in context_tys.iter_mut() {
            match &mut *context_ty.1 {
                // Hfun_context in the AST.
                Ty_::Tapply((_, name), _) if name.starts_with('$') => {
                    if let Some((param_ty, _)) = ty_by_param.get_mut(name.as_str()) {
                        // XXX rewrite param_ty without cloning?
                        *param_ty = self.rewrite_fun_ctx(&mut tparams, param_ty.clone(), name);
                    }
                }
                Ty_::Taccess(TaccessType(Ty(_, box Ty_::Tapply((_, name), _)), cst)) => {
                    if let Some((param_ty, param_pos)) = ty_by_param.get_mut(name.as_str()) {
                        // XXX rewrite without cloning pos?
                        let mut rewrite = |t| {
                            rewrite_arg_ctx(
                                &mut tparams,
                                &mut where_constraints,
                                t,
                                param_pos.clone(),
                                name,
                                context_ty.0.clone(),
                                cst.clone(),
                            )
                        };
                        match &mut *param_ty.1 {
                            Ty_::Tlike(ty) => match ty {
                                Ty(r, box Ty_::Toption(tinner)) => {
                                    *ty = Ty(
                                        r.clone(),
                                        Box::new(Ty_::Toption(rewrite(tinner.clone()))),
                                    )
                                }
                                _ => {
                                    *ty = rewrite(ty.clone());
                                }
                            },
                            Ty_::Toption(ty) => {
                                *ty = rewrite(ty.clone());
                            }
                            _ => {
                                *param_ty = rewrite(param_ty.clone());
                            }
                        }
                    }
                }
                Ty_::Taccess(TaccessType(t, cst)) if Self::taccess_root_is_generic(t) => {
                    let left_id = (
                        context_ty.0.pos().cloned().unwrap_or(Pos::NONE),
                        self.ctx_generic_for_generic_taccess(t, cst.1.as_str()),
                    );
                    tparams.push(tp(left_id.clone(), vec![]));
                    let left = Ty(context_ty.0.clone(), Box::new(Ty_::Tgeneric(left_id.1)));
                    where_constraints.push(WhereConstraint(
                        left,
                        ConstraintKind::ConstraintEq,
                        context_ty.clone(),
                    ));
                }
                _ => {}
            }
        }

        let params = params
            .into_iter()
            .map(|param| match param.name {
                None => param,
                Some(ref name) => match ty_by_param.get(name) {
                    Some((type_, _)) if param.type_ != *type_ => FunParam {
                        type_: type_.clone(),
                        ..param
                    },
                    _ => param,
                },
            })
            .collect();

        // XXX rewrite as iter_mut
        let mut context_tys: Vec<_> = context_tys
            .into_iter()
            .map(|Ty(r, ty_)| {
                let ty_ = match *ty_ {
                    Ty_::Tapply((_, name), tys) if tys.is_empty() && name.starts_with('$') => {
                        Ty_::Tgeneric(self.ctx_generic_for_fun(&name))
                    }
                    Ty_::Taccess(TaccessType(Ty(_, box Ty_::Tapply((_, name), tys)), cst))
                        if tys.is_empty() && name.starts_with('$') =>
                    {
                        let name = self.ctx_generic_for_dependent(&name, &cst.1);
                        Ty_::Tgeneric(name)
                    }
                    Ty_::Taccess(TaccessType(t, cst)) if Self::taccess_root_is_generic(&t) => {
                        let name = self.ctx_generic_for_generic_taccess(&t, &cst.1);
                        Ty_::Tgeneric(name)
                    }
                    _ => return Ty(r, ty_),
                };
                Ty(r, Box::new(ty_))
            })
            .collect();
        let cap_ty = if context_tys.len() == 1 {
            context_tys.pop().unwrap()
        } else {
            Ty(cap_reason, Box::new(Ty_::Tintersection(context_tys)))
        };
        let implicit_params = FunImplicitParams {
            capability: CapTy(cap_ty),
        };

        (params, tparams, implicit_params, where_constraints)
    }
}

impl<'o, 't> FlattenSmartConstructors for DirectDeclSmartConstructors<'o, 't> {
    // type Output = Node in direct_decl_smart_constructors_generated_obr.rs

    fn flatten(&self, kind: SyntaxKind, lst: Vec<Self::Output>) -> Self::Output {
        let size = lst
            .iter()
            .map(|s| match s {
                Node::List(children) => children.len(),
                x => {
                    if Self::is_zero(x) {
                        0
                    } else {
                        1
                    }
                }
            })
            .sum();
        let mut r = Vec::with_capacity(size);
        for s in lst.into_iter() {
            match s {
                Node::List(children) => r.extend(children),
                x if !Self::is_zero(&x) => r.push(x),
                _ => {}
            }
        }
        match r.len() {
            0 => Node::Ignored(kind),
            1 => r.pop().unwrap(),
            _ => Node::List(r),
        }
    }

    fn zero(kind: SyntaxKind) -> Node {
        Node::Ignored(kind)
    }

    fn is_zero(s: &Self::Output) -> bool {
        match s {
            Node::Token(token) => match token.kind() {
                TokenKind::Yield | TokenKind::Required | TokenKind::Lateinit => false,
                _ => true,
            },
            Node::List(inner) => inner.iter().all(Self::is_zero),
            _ => true,
        }
    }

    fn make_token(&mut self, token: CompactToken) -> Self::Output {
        let token_text = |this: &Self| Self::str_from_utf8(this.token_bytes(&token)).into_owned();
        let token_pos = |this: &Self| {
            let start = this
                .source_text
                .offset_to_file_pos_triple(token.start_offset());
            let end = this
                .source_text
                .offset_to_file_pos_triple(token.end_offset());
            Pos::from_lnum_bol_offset(this.filename.clone(), start, end)
        };
        let kind = token.kind();

        let result = match kind {
            TokenKind::Name | TokenKind::XHPClassName => {
                let text = token_text(self);
                let pos = token_pos(self);

                let name = if kind == TokenKind::XHPClassName {
                    Node::XhpName(text, pos.clone())
                } else {
                    Node::Name(text, pos.clone())
                };

                if self.previous_token_kind == TokenKind::Class
                    || self.previous_token_kind == TokenKind::Trait
                    || self.previous_token_kind == TokenKind::Interface
                    || self.previous_token_kind == TokenKind::Enum
                {
                    if let Some(current_class_name) = self.elaborate_defined_id(&name) {
                        let previous_token_kind = self.previous_token_kind;
                        let this = Rc::make_mut(&mut self.state);
                        this.lexed_name_after_classish_keyword(
                            current_class_name.1,
                            pos,
                            previous_token_kind,
                        );
                    }
                }
                name
            }
            TokenKind::Variable => Node::Variable(token_text(self), token_pos(self)),
            // There are a few types whose string representations we have to
            // grab anyway, so just go ahead and treat them as generic names.
            TokenKind::Vec
            | TokenKind::Dict
            | TokenKind::Keyset
            | TokenKind::Tuple
            | TokenKind::Classname
            | TokenKind::SelfToken => Node::Name(token_text(self), token_pos(self)),
            TokenKind::XHPElementName => Node::XhpName(token_text(self), token_pos(self)),
            TokenKind::SingleQuotedStringLiteral => match escaper::unescape_single(
                &Self::str_from_utf8(escaper::unquote_slice(self.token_bytes(&token))),
            ) {
                Ok(text) => Node::StringLiteral(text.into(), token_pos(self)),
                Err(_) => Node::Ignored(SK::Token(kind)),
            },
            TokenKind::DoubleQuotedStringLiteral => match escaper::unescape_double(
                &Self::str_from_utf8(escaper::unquote_slice(self.token_bytes(&token))),
            ) {
                Ok(text) => Node::StringLiteral(text, token_pos(self)),
                Err(_) => Node::Ignored(SK::Token(kind)),
            },
            TokenKind::HeredocStringLiteral => match escaper::unescape_heredoc(
                &Self::str_from_utf8(escaper::unquote_slice(self.token_bytes(&token))),
            ) {
                Ok(text) => Node::StringLiteral(text, token_pos(self)),
                Err(_) => Node::Ignored(SK::Token(kind)),
            },
            TokenKind::NowdocStringLiteral => match escaper::unescape_nowdoc(&Self::str_from_utf8(
                escaper::unquote_slice(self.token_bytes(&token)),
            )) {
                Ok(text) => Node::StringLiteral(text.into(), token_pos(self)),
                Err(_) => Node::Ignored(SK::Token(kind)),
            },
            TokenKind::DecimalLiteral
            | TokenKind::OctalLiteral
            | TokenKind::HexadecimalLiteral
            | TokenKind::BinaryLiteral => Node::IntLiteral(token_text(self), token_pos(self)),
            TokenKind::FloatingLiteral => Node::FloatingLiteral(token_text(self), token_pos(self)),
            TokenKind::BooleanLiteral => Node::BooleanLiteral(token_text(self), token_pos(self)),
            TokenKind::String => self.hint_ty(token_pos(self), mk_string_ty_(token_pos(self))),
            TokenKind::Int => self.prim_ty(aast::Tprim::Tint, token_pos(self)),
            TokenKind::Float => self.prim_ty(aast::Tprim::Tfloat, token_pos(self)),
            // "double" and "boolean" are parse errors--they should be written
            // "float" and "bool". The decl-parser treats the incorrect names as
            // type names rather than primitives.
            TokenKind::Double | TokenKind::Boolean => self.hint_ty(
                token_pos(self),
                Ty_::Tapply((token_pos(self), token_text(self)), vec![]),
            ),
            TokenKind::Num => self.prim_ty(aast::Tprim::Tnum, token_pos(self)),
            TokenKind::Bool => self.prim_ty(aast::Tprim::Tbool, token_pos(self)),
            TokenKind::Mixed => {
                let reason = Reason::FromWitnessDecl(WitnessDecl::Hint(token_pos(self)));
                if self.implicit_sdt() {
                    let ty_ = self.make_supportdyn(token_pos(self), Ty_::Tmixed);
                    Node::Ty(Box::new(Ty(reason, Box::new(ty_))))
                } else {
                    Node::Ty(Box::new(Ty(reason, Box::new(Ty_::Tmixed))))
                }
            }
            TokenKind::Void => self.prim_ty(aast::Tprim::Tvoid, token_pos(self)),
            TokenKind::Arraykey => self.prim_ty(aast::Tprim::Tarraykey, token_pos(self)),
            TokenKind::Noreturn => self.prim_ty(aast::Tprim::Tnoreturn, token_pos(self)),
            TokenKind::Resource => self.prim_ty(aast::Tprim::Tresource, token_pos(self)),
            TokenKind::Class | TokenKind::Interface | TokenKind::Trait => {
                if self.under_no_auto_dynamic {
                    let this = Rc::make_mut(&mut self.state);
                    this.inside_no_auto_dynamic_class = true;
                }
                Node::Token(FixedWidthToken::new(kind, token.start_offset()))
            }
            TokenKind::NullLiteral
            | TokenKind::Darray
            | TokenKind::Varray
            | TokenKind::Backslash
            | TokenKind::Construct
            | TokenKind::LeftParen
            | TokenKind::RightParen
            | TokenKind::LeftBracket
            | TokenKind::RightBracket
            | TokenKind::Shape
            | TokenKind::Question
            | TokenKind::This
            | TokenKind::Tilde
            | TokenKind::Exclamation
            | TokenKind::Plus
            | TokenKind::Minus
            | TokenKind::PlusPlus
            | TokenKind::MinusMinus
            | TokenKind::At
            | TokenKind::Star
            | TokenKind::Slash
            | TokenKind::EqualEqual
            | TokenKind::EqualEqualEqual
            | TokenKind::StarStar
            | TokenKind::AmpersandAmpersand
            | TokenKind::BarBar
            | TokenKind::LessThan
            | TokenKind::LessThanEqual
            | TokenKind::GreaterThan
            | TokenKind::GreaterThanEqual
            | TokenKind::Dot
            | TokenKind::Ampersand
            | TokenKind::Bar
            | TokenKind::LessThanLessThan
            | TokenKind::GreaterThanGreaterThan
            | TokenKind::Percent
            | TokenKind::QuestionQuestion
            | TokenKind::Equal
            | TokenKind::Abstract
            | TokenKind::As
            | TokenKind::Super
            | TokenKind::Async
            | TokenKind::DotDotDot
            | TokenKind::Extends
            | TokenKind::Final
            | TokenKind::Implements
            | TokenKind::Inout
            | TokenKind::Newctx
            | TokenKind::Newtype
            | TokenKind::Type
            | TokenKind::Yield
            | TokenKind::Semicolon
            | TokenKind::Private
            | TokenKind::Protected
            | TokenKind::Public
            | TokenKind::Reify
            | TokenKind::Static
            | TokenKind::Lateinit
            | TokenKind::RightBrace
            | TokenKind::Enum
            | TokenKind::Const
            | TokenKind::Function
            | TokenKind::Namespace
            | TokenKind::XHP
            | TokenKind::Required
            | TokenKind::Ctx
            | TokenKind::Readonly
            | TokenKind::Internal
            | TokenKind::Global
            | TokenKind::Optional
            | TokenKind::Absent
            | TokenKind::Named
            | TokenKind::Nameof => Node::Token(FixedWidthToken::new(kind, token.start_offset())),
            TokenKind::Attribute
            | TokenKind::Await
            | TokenKind::Binary
            | TokenKind::Break
            | TokenKind::Case
            | TokenKind::Catch
            | TokenKind::Category
            | TokenKind::Children
            | TokenKind::Clone
            | TokenKind::Concurrent
            | TokenKind::Continue
            | TokenKind::Default
            | TokenKind::Delay
            | TokenKind::Do
            | TokenKind::Echo
            | TokenKind::Else
            | TokenKind::Empty
            | TokenKind::EndOfFile
            | TokenKind::Endif
            | TokenKind::Eval
            | TokenKind::Fallthrough
            | TokenKind::File
            | TokenKind::Finally
            | TokenKind::For
            | TokenKind::Foreach
            | TokenKind::If
            | TokenKind::Include
            | TokenKind::Include_once
            | TokenKind::Instanceof
            | TokenKind::Insteadof
            | TokenKind::Integer
            | TokenKind::Is
            | TokenKind::Isset
            | TokenKind::List
            | TokenKind::Match
            | TokenKind::Module
            | TokenKind::New
            | TokenKind::Parent
            | TokenKind::Print
            | TokenKind::Real
            | TokenKind::Require
            | TokenKind::Require_once
            | TokenKind::Return
            | TokenKind::Switch
            | TokenKind::Throw
            | TokenKind::Try
            | TokenKind::Unset
            | TokenKind::Upcast
            | TokenKind::Use
            | TokenKind::Using
            | TokenKind::Var
            | TokenKind::With
            | TokenKind::Where
            | TokenKind::While
            | TokenKind::LeftBrace
            | TokenKind::MinusGreaterThan
            | TokenKind::Dollar
            | TokenKind::LessThanEqualGreaterThan
            | TokenKind::ExclamationEqual
            | TokenKind::ExclamationEqualEqual
            | TokenKind::Carat
            | TokenKind::QuestionAs
            | TokenKind::QuestionColon
            | TokenKind::QuestionQuestionEqual
            | TokenKind::Colon
            | TokenKind::StarStarEqual
            | TokenKind::StarEqual
            | TokenKind::SlashEqual
            | TokenKind::PercentEqual
            | TokenKind::PlusEqual
            | TokenKind::MinusEqual
            | TokenKind::DotEqual
            | TokenKind::LessThanLessThanEqual
            | TokenKind::GreaterThanGreaterThanEqual
            | TokenKind::AmpersandEqual
            | TokenKind::CaratEqual
            | TokenKind::BarEqual
            | TokenKind::Comma
            | TokenKind::ColonColon
            | TokenKind::EqualGreaterThan
            | TokenKind::EqualEqualGreaterThan
            | TokenKind::QuestionMinusGreaterThan
            | TokenKind::DollarDollar
            | TokenKind::BarGreaterThan
            | TokenKind::BarQuestionGreaterThan
            | TokenKind::SlashGreaterThan
            | TokenKind::LessThanSlash
            | TokenKind::LessThanQuestion
            | TokenKind::Backtick
            | TokenKind::Hash
            | TokenKind::Package
            | TokenKind::Let
            | TokenKind::ErrorToken
            | TokenKind::DoubleQuotedStringLiteralHead
            | TokenKind::StringLiteralBody
            | TokenKind::DoubleQuotedStringLiteralTail
            | TokenKind::HeredocStringLiteralHead
            | TokenKind::HeredocStringLiteralTail
            | TokenKind::XHPCategoryName
            | TokenKind::XHPStringLiteral
            | TokenKind::XHPBody
            | TokenKind::XHPComment
            | TokenKind::Hashbang => {
                if kind.fixed_width().is_some() {
                    Node::IgnoredToken(FixedWidthToken::new(kind, token.start_offset()))
                } else {
                    Node::Ignored(SK::Token(kind))
                }
            }
        };
        self.previous_token_kind = kind;
        result
    }

    fn make_error(&mut self, error: Self::Output) -> Self::Output {
        // If it's a Token or IgnoredToken, we can use it for error recovery.
        // For instance, in `function using() {}`, the `using` keyword will be a
        // token wrapped in an error CST node, since the keyword isn't legal in
        // that position.
        error
    }

    fn make_missing(&mut self, offset: usize) -> Self::Output {
        Node::Missing(offset)
    }

    fn make_list(&mut self, mut items: Vec<Self::Output>, _: usize) -> Self::Output {
        if let Some(yield_) = items
            .iter()
            .flat_map(|node| node.iter())
            .find(|node| node.is_token(TokenKind::Yield))
        {
            return yield_.clone();
        }
        items.retain(|node| node.is_present());
        if items.is_empty() {
            Node::Ignored(SK::SyntaxList)
        } else {
            Node::List(items)
        }
    }

    fn make_qualified_name(&mut self, parts: Self::Output) -> Self::Output {
        let pos = self.get_pos(&parts);
        match parts {
            Node::List(nodes) => Node::QualifiedName(nodes, pos),
            node if node.is_ignored() => Node::Ignored(SK::QualifiedName),
            node => Node::QualifiedName(vec![node], pos),
        }
    }

    fn make_module_name(&mut self, parts: Self::Output) -> Self::Output {
        let pos = self.get_pos(&parts);
        match parts {
            Node::List(nodes) => Node::ModuleName(nodes, pos),
            node if node.is_ignored() => Node::Ignored(SK::ModuleName),
            node => Node::ModuleName(vec![node], pos),
        }
    }

    fn make_simple_type_specifier(&mut self, specifier: Self::Output) -> Self::Output {
        // Return this explicitly because flatten filters out zero nodes, and
        // we treat most non-error nodes as zeroes.
        specifier
    }

    fn make_literal_expression(&mut self, expression: Self::Output) -> Self::Output {
        expression
    }

    fn make_simple_initializer(
        &mut self,
        equals: Self::Output,
        expr: Self::Output,
    ) -> Self::Output {
        // If the expr is Ignored, bubble up the assignment operator so that we
        // can tell that *some* initializer was here. Useful for class
        // properties, where we need to enforce that properties without default
        // values are initialized in the constructor.
        if expr.is_ignored() { equals } else { expr }
    }

    fn make_anonymous_function(
        &mut self,
        _attribute_spec: Self::Output,
        _async_keyword: Self::Output,
        _function_keyword: Self::Output,
        _type_parameters: Self::Output,
        _left_paren: Self::Output,
        _parameters: Self::Output,
        _right_paren: Self::Output,
        _ctx_list: Self::Output,
        _colon: Self::Output,
        _readonly_return: Self::Output,
        _type_: Self::Output,
        _use_: Self::Output,
        _body: Self::Output,
    ) -> Self::Output {
        // do not allow Yield to bubble up
        Node::Ignored(SK::AnonymousFunction)
    }

    fn make_lambda_expression(
        &mut self,
        _attribute_spec: Self::Output,
        _async_: Self::Output,
        _signature: Self::Output,
        _arrow: Self::Output,
        _body: Self::Output,
    ) -> Self::Output {
        // do not allow Yield to bubble up
        Node::Ignored(SK::LambdaExpression)
    }

    fn make_awaitable_creation_expression(
        &mut self,
        _attribute_spec: Self::Output,
        _async_: Self::Output,
        _compound_statement: Self::Output,
    ) -> Self::Output {
        // do not allow Yield to bubble up
        Node::Ignored(SK::AwaitableCreationExpression)
    }

    fn make_element_initializer(
        &mut self,
        key: Self::Output,
        _arrow: Self::Output,
        value: Self::Output,
    ) -> Self::Output {
        Node::ListItem(Box::new(key), Box::new(value))
    }

    fn make_prefix_unary_expression(
        &mut self,
        op: Self::Output,
        value: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&op, &value);
        let op = match op.token_kind() {
            Some(TokenKind::Tilde) => Uop::Utild,
            Some(TokenKind::Exclamation) => Uop::Unot,
            Some(TokenKind::Plus) => Uop::Uplus,
            Some(TokenKind::Minus) => Uop::Uminus,
            Some(TokenKind::PlusPlus) => Uop::Uincr,
            Some(TokenKind::MinusMinus) => Uop::Udecr,
            Some(TokenKind::At) => Uop::Usilence,
            _ => return Node::Ignored(SK::PrefixUnaryExpression),
        };
        match self.node_to_expr(value) {
            Either::Left(value) => Node::Expr(Box::new(aast::Expr(
                (),
                pos,
                aast::Expr_::Unop(Box::new((op, value))),
            ))),
            Either::Right(_) => Node::Ignored(SK::PrefixUnaryExpression),
        }
    }

    fn make_postfix_unary_expression(
        &mut self,
        value: Self::Output,
        op: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&value, &op);
        let op = match op.token_kind() {
            Some(TokenKind::PlusPlus) => Uop::Upincr,
            Some(TokenKind::MinusMinus) => Uop::Updecr,
            _ => return Node::Ignored(SK::PostfixUnaryExpression),
        };
        match self.node_to_expr(value) {
            Either::Left(value) => Node::Expr(Box::new(aast::Expr(
                (),
                pos,
                aast::Expr_::Unop(Box::new((op, value))),
            ))),
            Either::Right(_) => Node::Ignored(SK::PostfixUnaryExpression),
        }
    }

    fn make_binary_expression(
        &mut self,
        lhs: Self::Output,
        op_node: Self::Output,
        rhs: Self::Output,
    ) -> Self::Output {
        let t = op_node.token_kind();
        let op = if matches!(t, Some(TokenKind::Equal)) {
            None
        } else {
            Some(match op_node.token_kind() {
                Some(TokenKind::Plus) => Bop::Plus,
                Some(TokenKind::Minus) => Bop::Minus,
                Some(TokenKind::Star) => Bop::Star,
                Some(TokenKind::Slash) => Bop::Slash,
                Some(TokenKind::EqualEqual) => Bop::Eqeq,
                Some(TokenKind::EqualEqualEqual) => Bop::Eqeqeq,
                Some(TokenKind::StarStar) => Bop::Starstar,
                Some(TokenKind::AmpersandAmpersand) => Bop::Ampamp,
                Some(TokenKind::BarBar) => Bop::Barbar,
                Some(TokenKind::LessThan) => Bop::Lt,
                Some(TokenKind::LessThanEqual) => Bop::Lte,
                Some(TokenKind::LessThanLessThan) => Bop::Ltlt,
                Some(TokenKind::GreaterThan) => Bop::Gt,
                Some(TokenKind::GreaterThanEqual) => Bop::Gte,
                Some(TokenKind::GreaterThanGreaterThan) => Bop::Gtgt,
                Some(TokenKind::Dot) => Bop::Dot,
                Some(TokenKind::Ampersand) => Bop::Amp,
                Some(TokenKind::Bar) => Bop::Bar,
                Some(TokenKind::Percent) => Bop::Percent,
                Some(TokenKind::QuestionQuestion) => Bop::QuestionQuestion,
                _ => return Node::Ignored(SK::BinaryExpression),
            })
        };

        match (&op, rhs.is_token(TokenKind::Yield)) {
            (None, true) => return rhs,
            _ => {}
        }

        let pos = Self::merge(self.merge_positions(&lhs, &op_node), self.get_pos(&rhs));

        let lhs = match self.node_to_expr(lhs) {
            Either::Left(lhs) => lhs,
            Either::Right(_) => return Node::Ignored(SK::BinaryExpression),
        };
        let rhs = match self.node_to_expr(rhs) {
            Either::Left(rhs) => rhs,
            Either::Right(_) => return Node::Ignored(SK::BinaryExpression),
        };
        match op {
            None => Node::Expr(Box::new(aast::Expr(
                (),
                pos,
                aast::Expr_::Assign(Box::new((lhs, op, rhs))),
            ))),
            Some(op) => Node::Expr(Box::new(aast::Expr(
                (),
                pos,
                aast::Expr_::Binop(Box::new(aast::Binop { bop: op, lhs, rhs })),
            ))),
        }
    }

    fn make_parenthesized_expression(
        &mut self,
        _lparen: Self::Output,
        expr: Self::Output,
        _rparen: Self::Output,
    ) -> Self::Output {
        expr
    }

    fn make_enum_class_label_expression(
        &mut self,
        _class: Self::Output,
        _hash: Self::Output,
        label: Self::Output,
    ) -> Self::Output {
        // In case we want it later on, _class is either Ignored(Missing)
        // or a Name node, like label
        match label {
            Node::Name(lbl, _) => Node::EnumClassLabel(lbl),
            _ => Node::Ignored(SK::EnumClassLabelExpression),
        }
    }

    fn make_list_item(&mut self, item: Self::Output, sep: Self::Output) -> Self::Output {
        match (item.is_ignored(), sep.is_ignored()) {
            (true, true) => Node::Ignored(SK::ListItem),
            (false, true) => item,
            (true, false) => sep,
            (false, false) => Node::ListItem(Box::new(item), Box::new(sep)),
        }
    }

    fn make_type_arguments(
        &mut self,
        less_than: Self::Output,
        arguments: Self::Output,
        greater_than: Self::Output,
    ) -> Self::Output {
        Node::BracketedList(Box::new((
            self.get_pos(&less_than),
            arguments.into_vec(),
            self.get_pos(&greater_than),
        )))
    }

    fn make_generic_type_specifier(
        &mut self,
        class_type: Self::Output,
        type_arguments: Self::Output,
    ) -> Self::Output {
        let class_id = match self.expect_name(&class_type) {
            Some(id) => id,
            None => return Node::Ignored(SK::GenericTypeSpecifier),
        };
        match class_id.1.trim_start_matches('\\') {
            "varray_or_darray" | "vec_or_dict" => {
                let id_pos = class_id.0;
                let pos = Self::merge(id_pos.clone(), self.get_pos(&type_arguments));
                let mut type_arguments = type_arguments.into_vec();
                let ty_ = match type_arguments.len() {
                    2 => {
                        let tv = type_arguments.pop().unwrap();
                        let tk = type_arguments.pop().unwrap();
                        Ty_::TvecOrDict(
                            self.node_to_ty(tk)
                                .unwrap_or_else(|| self.tany_with_pos(id_pos.clone())),
                            self.node_to_ty(tv)
                                .unwrap_or_else(|| self.tany_with_pos(id_pos.clone())),
                        )
                    }
                    1 => {
                        let tv = type_arguments.pop().unwrap();
                        Ty_::TvecOrDict(
                            self.vec_or_dict_key(pos.clone()),
                            self.node_to_ty(tv)
                                .unwrap_or_else(|| self.tany_with_pos(id_pos.clone())),
                        )
                    }
                    _ => TANY_,
                };
                self.hint_ty(pos, ty_)
            }
            _ => {
                let Id(pos, class_type) = class_id;
                match class_type.rsplit('\\').next() {
                    Some(name) if self.is_type_param_in_scope(name) => {
                        let ty_ = Ty_::Tgeneric(name.into());
                        self.hint_ty(pos, ty_)
                    }
                    _ => {
                        let class_type = self.elaborate_raw_id(&class_type);
                        let ta_pos = self.get_pos(&type_arguments);
                        self.make_apply((pos, class_type.into()), type_arguments, ta_pos)
                    }
                }
            }
        }
    }

    fn make_alias_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        module_kw_opt: Self::Output,
        keyword: Self::Output,
        name: Self::Output,
        generic_params: Self::Output,
        constraint: Self::Output,
        _equal: Self::Output,
        aliased_type: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        if name.is_ignored() {
            return Node::Ignored(SK::AliasDeclaration);
        }
        let Id(pos, name) = match self.elaborate_defined_id(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::AliasDeclaration),
        };
        let ty = match self.node_to_ty(aliased_type) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::AliasDeclaration),
        };
        let mut as_constraint = None;
        let mut super_constraint = None;
        for c in constraint.into_iter() {
            if let Node::TypeConstraint(kind, hint) = c {
                let ty = self.node_to_ty(*hint);
                match kind {
                    ConstraintKind::ConstraintAs => as_constraint = ty,
                    ConstraintKind::ConstraintSuper => super_constraint = ty,
                    _ => {}
                }
            }
        }

        // Pop the type params stack only after creating all inner types.
        let tparams = self.pop_type_params(generic_params);

        let mut docs_url = None;
        for attribute in attributes.iter() {
            match attribute {
                Node::Attribute(box attr) => {
                    if attr.name.1 == "__Docs" {
                        if let Some((_, bstr)) = &attr.string_literal_param {
                            docs_url = Some(Self::str_from_utf8(bstr).into_owned());
                        }
                    }
                }
                _ => {}
            }
        }

        // Parse the user attributes
        // in facts-mode all attributes are saved, otherwise only
        // __NoAutoDynamic/__NoAutoLikes/__Overlapping and Sealed are
        let user_attributes = attributes
            .into_iter()
            .rev()
            .filter_map(|attribute| {
                if let Node::Attribute(attr) = attribute {
                    if self.keep_user_attribute(&attr) {
                        Some(self.user_attribute_to_decl(*attr))
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .collect();

        let internal = modifiers.iter().any(|m| {
            m.as_visibility() == Some(aast::Visibility::Internal)
                || m.as_visibility() == Some(aast::Visibility::ProtectedInternal)
        });
        let is_module_newtype = module_kw_opt.is_ignored_token_with_kind(TokenKind::Module);
        let vis = match keyword.token_kind() {
            Some(TokenKind::Type) => aast::TypedefVisibility::Transparent,
            Some(TokenKind::Newtype) if is_module_newtype => aast::TypedefVisibility::OpaqueModule,
            Some(TokenKind::Newtype) => aast::TypedefVisibility::Opaque,
            _ => aast::TypedefVisibility::Transparent,
        };
        let typedef = TypedefType {
            module: self.module.clone(),
            pos,
            tparams,
            as_constraint,
            super_constraint,
            type_assignment: TypedefTypeAssignment::SimpleTypeDef(vis, ty),
            is_ctx: false,
            attributes: user_attributes,
            internal,
            docs_url,
            package: self.package.clone(),
        };

        let this = Rc::make_mut(&mut self.state);
        this.typedef_decl(name, typedef)
    }

    fn make_context_alias_declaration(
        &mut self,
        attributes: Self::Output,
        _keyword: Self::Output,
        name: Self::Output,
        generic_params: Self::Output,
        constraint: Self::Output,
        _equal: Self::Output,
        ctx_list: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        if name.is_ignored() {
            return Node::Ignored(SK::ContextAliasDeclaration);
        }
        let Id(pos, name) = match self.elaborate_defined_id(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::ContextAliasDeclaration),
        };
        let ty = match self.node_to_ty(ctx_list) {
            Some(ty) => ty,
            None => Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(Ty_::Tapply(
                    (pos.clone(), "\\HH\\Contexts\\defaults".into()),
                    vec![],
                )),
            ),
        };

        // lowerer ensures there is only one as constraint
        let mut as_constraint = None;
        let mut super_constraint = None;
        for c in constraint.into_iter() {
            if let Node::ContextConstraint(kind, hint) = c {
                let ty = self.node_to_ty(*hint);
                match kind {
                    ConstraintKind::ConstraintAs => as_constraint = ty,
                    ConstraintKind::ConstraintSuper => super_constraint = ty,
                    _ => {}
                }
            }
        }
        // Pop the type params stack only after creating all inner types.
        let tparams = self.pop_type_params(generic_params);
        let user_attributes = if self.opts.keep_user_attributes {
            attributes
                .into_iter()
                .rev()
                .filter_map(|attribute| {
                    if let Node::Attribute(box attr) = attribute {
                        Some(self.user_attribute_to_decl(attr))
                    } else {
                        None
                    }
                })
                .collect()
        } else {
            vec![]
        };
        let typedef = TypedefType {
            module: self.module.clone(),
            pos,
            tparams,
            as_constraint,
            super_constraint,
            type_assignment: TypedefTypeAssignment::SimpleTypeDef(
                aast::TypedefVisibility::Opaque,
                ty,
            ),
            is_ctx: true,
            attributes: user_attributes,
            internal: false,
            docs_url: None,
            package: self.package.clone(),
        };

        let this = Rc::make_mut(&mut self.state);
        this.typedef_decl(name, typedef)
    }

    fn make_case_type_declaration(
        &mut self,
        attribute_spec: Self::Output,
        modifiers: Self::Output,
        _case_keyword: Self::Output,
        _type_keyword: Self::Output,
        name: Self::Output,
        generic_parameter: Self::Output,
        _as: Self::Output,
        bounds: Self::Output,
        _equal: Self::Output,
        variants: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        if name.is_ignored() {
            return Node::Ignored(SK::CaseTypeDeclaration);
        }
        let Id(pos, name) = match self.elaborate_defined_id(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::CaseTypeDeclaration),
        };

        let as_constraint = match bounds.len() {
            0 => None,
            1 => self.node_to_ty(bounds.into_iter().next().unwrap()),
            _ => {
                let pos = self.get_pos(&bounds);
                let tys = bounds
                    .into_iter()
                    .filter_map(|x| match x {
                        Node::ListItem(ty, _commas) => self.node_to_ty(*ty),
                        x => self.node_to_ty(x),
                    })
                    .collect();
                Some(Ty(
                    Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
                    Box::new(Ty_::Tintersection(tys)),
                ))
            }
        };
        let mut variants = variants.into_iter().filter_map(|x| match x {
            Node::CaseTypeVariantWithWhereClause(box (ty, constraints)) => self
                .node_to_ty(ty)
                .map(|ty| TypedefCaseTypeVariant(ty, constraints)),
            _ => self
                .node_to_ty(x)
                .map(|ty| TypedefCaseTypeVariant(ty, vec![])),
        });
        let variant;
        if let Some(v) = variants.next() {
            variant = v;
        } else {
            return Node::Ignored(SK::CaseTypeDeclaration);
        }
        let type_assignment = TypedefTypeAssignment::CaseType(variant, variants.collect());

        // Pop the type params stack only after creating all inner types.
        let tparams = self.pop_type_params(generic_parameter);

        let mut docs_url = None;
        for attribute in attribute_spec.iter() {
            match attribute {
                Node::Attribute(attr) => {
                    if attr.name.1 == "__Docs" {
                        if let Some((_, bstr)) = &attr.string_literal_param {
                            docs_url = Some(Self::str_from_utf8(bstr).into_owned());
                        }
                    }
                }
                _ => {}
            }
        }

        // Parse the user attributes
        // in facts-mode all attributes are saved, otherwise only __NoAutoDynamic/__NoAutoLikes is
        let user_attributes = attribute_spec
            .into_iter()
            .rev()
            .filter_map(|attribute| {
                if let Node::Attribute(attr) = attribute {
                    if self.keep_user_attribute(&attr) {
                        Some(self.user_attribute_to_decl(*attr))
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .collect();

        let internal = modifiers.iter().any(|m| {
            m.as_visibility() == Some(aast::Visibility::Internal)
                || m.as_visibility() == Some(aast::Visibility::ProtectedInternal)
        });
        let typedef = TypedefType {
            module: self.module.clone(),
            pos,
            tparams,
            as_constraint,
            super_constraint: None,
            type_assignment,
            is_ctx: false,
            attributes: user_attributes,
            internal,
            docs_url,
            package: self.package.clone(),
        };

        let this = Rc::make_mut(&mut self.state);
        this.typedef_decl(name, typedef)
    }

    fn make_case_type_variant(
        &mut self,
        _bar: Self::Output,
        type_: Self::Output,
        where_: Self::Output,
    ) -> Self::Output {
        if type_.is_ignored() {
            Node::Ignored(SK::CaseTypeVariant)
        } else {
            let where_constraints: Vec<_> = where_
                .into_iter()
                .filter_map(|x| match x {
                    Node::WhereConstraint(x) => Some(*x),
                    _ => None,
                })
                .collect();
            if where_constraints.is_empty() {
                type_
            } else {
                Node::CaseTypeVariantWithWhereClause(Box::new((type_, where_constraints)))
            }
        }
    }

    fn make_type_constraint(&mut self, kind: Self::Output, value: Self::Output) -> Self::Output {
        let kind = match kind.token_kind() {
            Some(TokenKind::As) => ConstraintKind::ConstraintAs,
            Some(TokenKind::Super) => ConstraintKind::ConstraintSuper,
            _ => return Node::Ignored(SK::TypeConstraint),
        };
        Node::TypeConstraint(kind, Box::new(value))
    }

    fn make_context_constraint(&mut self, kind: Self::Output, value: Self::Output) -> Self::Output {
        let kind = match kind.token_kind() {
            Some(TokenKind::As) => ConstraintKind::ConstraintAs,
            Some(TokenKind::Super) => ConstraintKind::ConstraintSuper,
            _ => return Node::Ignored(SK::ContextConstraint),
        };
        Node::ContextConstraint(kind, Box::new(value))
    }

    fn make_type_parameter(
        &mut self,
        user_attributes: Self::Output,
        reify: Self::Output,
        variance: Self::Output,
        name: Self::Output,
        constraints: Self::Output,
    ) -> Self::Output {
        let user_attributes = match user_attributes {
            Node::BracketedList(box (_, attributes, _)) => attributes
                .into_iter()
                .filter_map(|x| match x {
                    Node::Attribute(box a) => Some(a),
                    _ => None,
                })
                .collect(),
            _ => vec![],
        };

        let constraints = constraints
            .into_iter()
            .filter_map(|node| match node {
                Node::TypeConstraint(kind, constraint) => Some((kind, *constraint)),
                _ => None,
            })
            .collect();

        Node::TypeParameter(Box::new(TypeParameterDecl {
            name,
            variance: match variance.token_kind() {
                Some(TokenKind::Minus) => Variance::Contravariant,
                Some(TokenKind::Plus) => Variance::Covariant,
                _ => Variance::Invariant,
            },
            reified: if reify.is_token(TokenKind::Reify) {
                if user_attributes.iter().any(|node| node.name.1 == "__Soft") {
                    aast::ReifyKind::SoftReified
                } else {
                    aast::ReifyKind::Reified
                }
            } else {
                aast::ReifyKind::Erased
            },
            constraints,
            user_attributes,
        }))
    }

    fn make_type_parameters(
        &mut self,
        _lt: Self::Output,
        tparams: Self::Output,
        _gt: Self::Output,
    ) -> Self::Output {
        let size = tparams.len();
        let mut tparams_with_name = Vec::with_capacity(size);
        let mut tparam_names = SSet::new();
        for node in tparams.into_iter() {
            match node {
                Node::TypeParameter(box decl) => {
                    let name = match decl.name.as_id() {
                        Some(name) => name,
                        None => return Node::Ignored(SK::TypeParameters),
                    };
                    tparam_names.insert(name.1.clone());
                    tparams_with_name.push((decl, name));
                }
                _ => {}
            }
        }
        let this = Rc::make_mut(&mut self.state);
        Rc::make_mut(&mut this.type_parameters).push(tparam_names);
        let mut tparams = Vec::with_capacity(tparams_with_name.len());
        for (decl, name) in tparams_with_name.into_iter() {
            let TypeParameterDecl {
                name: _,
                variance,
                reified,
                constraints,
                user_attributes,
            } = decl;
            let constraints = constraints
                .into_iter()
                .filter_map(|(kind, ty)| {
                    let ty = self.node_to_ty(ty)?;
                    let ty = self.convert_tapply_to_tgeneric(ty);
                    Some((kind, ty))
                })
                .collect();

            let user_attributes = user_attributes
                .into_iter()
                .rev()
                .map(|x| self.user_attribute_to_decl(x))
                .collect();
            tparams.push(Tparam {
                variance,
                name: name.into(),
                constraints,
                reified,
                user_attributes,
            });
        }
        Node::TypeParameters(tparams)
    }

    fn make_parameter_declaration(
        &mut self,
        attributes: Self::Output,
        visibility: Self::Output,
        optional: Self::Output,
        inout: Self::Output,
        named: Self::Output,
        readonly: Self::Output,
        pre_ellipsis: Self::Output,
        hint: Self::Output,
        ellipsis: Self::Output,
        name: Self::Output,
        initializer: Self::Output,
        parameter_end: Self::Output,
    ) -> Self::Output {
        let variadic = ellipsis.is_present();
        let (pos, name) = match name.as_variable() {
            Some(id) => {
                let Id(pos, name) = id;
                (pos, Some(name))
            }
            None if variadic => (self.get_pos(&ellipsis), None),
            None => return Node::Ignored(SK::ParameterDeclaration),
        };
        let is_splat = pre_ellipsis.is_token(TokenKind::DotDotDot);
        let kind = if inout.is_token(TokenKind::Inout) {
            ParamMode::FPinout
        } else {
            ParamMode::FPnormal
        };
        let is_readonly = readonly.is_token(TokenKind::Readonly);
        let is_named = named.is_token(TokenKind::Named);
        let hint = if self.opts.interpret_soft_types_as_like_types {
            let attributes = self.to_attributes(&attributes);
            if attributes.soft {
                let hint_pos = self.get_pos(&hint);
                match hint {
                    Node::Ty(box ty) => self.hint_ty(hint_pos, Ty_::Tlike(ty)),
                    _ => hint,
                }
            } else {
                hint
            }
        } else {
            hint
        };
        Node::FunParam(Box::new(FunParamDecl {
            attributes,
            visibility,
            kind,
            optional: optional.is_present() || initializer.is_present(),
            readonly: is_readonly,
            named: is_named,
            hint,
            pos,
            name,
            variadic,
            splat: is_splat,
            initializer,
            parameter_end,
        }))
    }

    fn make_function_declaration(
        &mut self,
        attributes: Self::Output,
        header: Self::Output,
        body: Self::Output,
    ) -> Self::Output {
        let parsed_attributes = self.to_attributes(&attributes);
        match header {
            Node::FunctionHeader(box header) => {
                let is_method = false;
                let internal = header.modifiers.iter().any(|m| {
                    m.as_visibility() == Some(aast::Visibility::Internal)
                        || m.as_visibility() == Some(aast::Visibility::ProtectedInternal)
                });
                let ((pos, name), type_, _) =
                    match self.function_to_ty(is_method, &attributes, header, body) {
                        Some(x) => x,
                        None => return Node::Ignored(SK::FunctionDeclaration),
                    };
                if self.state.opts.ignore_string_methods
                    && pos.filename().is_hhi()
                    && pos.filename().path_str() == "string.hhi"
                {
                    return Node::Ignored(SK::FunctionDeclaration);
                }
                let deprecated = parsed_attributes.deprecated.map(|msg| {
                    format!(
                        "The function {} is deprecated: {msg}",
                        name.trim_start_matches('\\'),
                    )
                });
                let fun_elt = FunElt {
                    module: self.module.clone(),
                    internal,
                    deprecated,
                    type_,
                    pos,
                    php_std_lib: parsed_attributes.php_std_lib,
                    support_dynamic_type: (self.implicit_sdt()
                        && !parsed_attributes.no_support_dynamic_type)
                        || parsed_attributes.support_dynamic_type
                        || parsed_attributes.dynamically_callable,
                    no_auto_dynamic: self.under_no_auto_dynamic,
                    no_auto_likes: parsed_attributes.no_auto_likes,
                    gated_by_feature_flag: parsed_attributes.gated_by_feature_flag.clone(),
                    package: self.package.clone(),
                    package_requirement: parsed_attributes
                        .require_package
                        .unwrap_or(PackageRequirement::RPNormal),
                };
                let function_attributes = if self.opts.keep_user_attributes {
                    attributes
                        .into_iter()
                        .rev()
                        .filter_map(|attribute| match attribute {
                            Node::Attribute(box attr) => Some(self.user_attribute_to_decl(attr)),
                            _ => None,
                        })
                        .collect()
                } else {
                    Vec::new()
                };
                let this = Rc::make_mut(&mut self.state);
                if !function_attributes.is_empty() {
                    this.function_attributes
                        .insert(name.clone(), function_attributes);
                }
                this.fun_decl(name, fun_elt)
            }
            _ => Node::Ignored(SK::FunctionDeclaration),
        }
    }

    fn make_contexts(
        &mut self,
        left_bracket: Self::Output,
        tys: Self::Output,
        right_bracket: Self::Output,
    ) -> Self::Output {
        let tys = tys
            .into_iter()
            .filter_map(|ty| match ty {
                Node::ListItem(box ty, _) | ty => {
                    // A wildcard is used for the context of a closure type on a
                    // parameter of a function with a function context (e.g.,
                    // `function f((function ()[_]: void) $f)[ctx $f]: void {}`).
                    match self.expect_name(&ty) {
                        Some(Id(pos, name)) if &name == "_" => {
                            return Some(Ty(
                                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                                Box::new(Ty_::Tapply((pos, name), vec![])),
                            ));
                        }
                        _ => {}
                    };
                    let ty = self.node_to_ty(ty)?;
                    match *ty.1 {
                        // Only three forms of type can appear here in a valid program:
                        //   - function contexts (`ctx $f`)
                        //   - value-dependent paths (`$v::C`)
                        //   - built-in contexts (`rx`, `cipp_of<EntFoo>`)
                        // The first and last will be represented with `Tapply`,
                        // but function contexts will use a variable name
                        // (containing a `$`). Built-in contexts are always in the
                        // \HH\Contexts namespace, so we rewrite those names here.
                        Ty_::Tapply((pos, name), targs) if !name.starts_with('$') => {
                            // The name will have been elaborated in the current
                            // namespace, but we actually want it to be in the
                            // \HH\Contexts namespace. Grab the last component of
                            // the name, and rewrite it in the correct namespace.
                            // Note that this makes it impossible to express names
                            // in any sub-namespace of \HH\Contexts (e.g.,
                            // "Unsafe\\cipp" will be rewritten as
                            // "\\HH\\Contexts\\cipp" rather than
                            // "\\HH\\Contexts\\Unsafe\\cipp").
                            let name = match name.trim_end_matches('\\').split('\\').next_back() {
                                Some(ctxname) => match ctxname.chars().next() {
                                    Some(first_char) if first_char.is_lowercase() => {
                                        format!("\\HH\\Contexts\\{ctxname}")
                                    }
                                    Some(_) | None => name,
                                },
                                None => name,
                            };
                            Some(Ty(ty.0, Box::new(Ty_::Tapply((pos, name), targs))))
                        }
                        _ => Some(ty),
                    }
                }
            })
            .collect();
        /* Like in as_fun_implicit_params, we keep the intersection as is: we do not simplify
         * empty or singleton intersections.
         */
        let pos = self.merge_positions(&left_bracket, &right_bracket);
        self.hint_ty(pos, Ty_::Tintersection(tys))
    }

    fn make_function_ctx_type_specifier(
        &mut self,
        ctx_keyword: Self::Output,
        variable: Self::Output,
    ) -> Self::Output {
        match variable.as_variable() {
            Some(Id(pos, name)) => {
                Node::Variable(name, Self::merge(pos, self.get_pos(&ctx_keyword)))
            }
            None => Node::Ignored(SK::FunctionCtxTypeSpecifier),
        }
    }

    fn make_function_declaration_header(
        &mut self,
        modifiers: Self::Output,
        _keyword: Self::Output,
        name: Self::Output,
        type_params: Self::Output,
        left_paren: Self::Output,
        param_list: Self::Output,
        _right_paren: Self::Output,
        capability: Self::Output,
        _colon: Self::Output,
        readonly_return: Self::Output,
        ret_hint: Self::Output,
        where_constraints: Self::Output,
    ) -> Self::Output {
        // Use the position of the left paren if the name is missing.
        // Keep the name if it's an IgnoredToken rather than an Ignored. An
        // IgnoredToken here should always be an error, but it's better to treat
        // a keyword as a name than to claim the function has no name at all.
        let name = if matches!(name, Node::Ignored(..) | Node::Missing(..)) {
            left_paren
        } else {
            name
        };
        Node::FunctionHeader(Box::new(FunctionHeader {
            name,
            modifiers,
            type_params,
            param_list,
            capability,
            ret_hint,
            readonly_return,
            where_constraints,
        }))
    }

    fn make_yield_expression(
        &mut self,
        keyword: Self::Output,
        _operand: Self::Output,
    ) -> Self::Output {
        assert!(keyword.token_kind() == Some(TokenKind::Yield));
        keyword
    }

    fn make_const_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        const_keyword: Self::Output,
        hint: Self::Output,
        decls: Self::Output,
        semicolon: Self::Output,
    ) -> Self::Output {
        match decls {
            // Class consts.
            Node::List(consts) if self.classish_name_builder.is_some() => {
                let ty = self.node_to_ty(hint);
                let is_shape_key = attributes
                    .contains_marker_attribute(naming_special_names::user_attributes::SHAPE_KEY);
                let should_record_value = is_shape_key && self.opts.include_const_shape_key_values;
                Node::List(
                    consts
                        .into_iter()
                        .filter_map(|cst| match cst {
                            Node::ConstInitializer(box (name, initializer, refs)) => {
                                let id = name.as_id()?;
                                let modifiers = read_member_modifiers(modifiers.iter());
                                let abstract_ = if modifiers.is_abstract {
                                    ClassConstKind::CCAbstract(!initializer.is_ignored())
                                } else {
                                    ClassConstKind::CCConcrete
                                };
                                let ty = match &ty {
                                    Some(ty) => ty.clone(),
                                    None => self
                                        .infer_const(name, initializer.clone())
                                        .unwrap_or_else(tany),
                                };
                                let const_value = if should_record_value {
                                    match self.const_value(&initializer) {
                                        Some(ConstValue::CVString(s)) if s == id.1 => {
                                            ConstValue::CVLabel
                                        }
                                        Some(ConstValue::CVConstAccess(..)) | None => {
                                            ConstValue::CVAbsent
                                        }
                                        Some(value) => value,
                                    }
                                } else {
                                    ConstValue::CVAbsent
                                };
                                Some(Node::Const(Box::new(
                                    shallow_decl_defs::ShallowClassConst {
                                        abstract_,
                                        name: id.into(),
                                        type_: ty,
                                        refs,
                                        value: if self.opts.include_assignment_values {
                                            Some(self.node_to_str(initializer, &semicolon))
                                        } else {
                                            None
                                        },
                                        is_shape_key,
                                        const_value,
                                    },
                                )))
                            }
                            _ => None,
                        })
                        .collect(),
                )
            }
            // Global consts.
            Node::List(consts) => {
                // Note: given "const int X=1,Y=2;", the legacy decl-parser
                // allows both decls, and it gives them both an identical text-span -
                // from start of "const" to end of semicolon. This is a bug but
                // the code here preserves it.
                let pos = self.merge_positions(&const_keyword, &semicolon);
                Node::List(
                    consts
                        .into_iter()
                        .filter_map(|cst| match cst {
                            Node::ConstInitializer(box (name, initializer, _refs)) => {
                                self.elaborate_defined_id(&name).map(|Id(id_pos, id)| {
                                    let ty = self
                                        .node_to_ty(hint.clone())
                                        .or_else(|| self.infer_const(name, initializer.clone()))
                                        .unwrap_or_else(|| self.tany_with_pos(id_pos));

                                    let value = if self.opts.include_assignment_values {
                                        Some(self.node_to_str(initializer, &semicolon))
                                    } else {
                                        None
                                    };

                                    let const_decl = ConstDecl {
                                        pos: pos.clone(),
                                        type_: ty,
                                        value,
                                        package: self.package.clone(),
                                    };
                                    let this = Rc::make_mut(&mut self.state);
                                    this.const_decl(id, const_decl)
                                })
                            }
                            _ => None,
                        })
                        .collect(),
                )
            }
            _ => Node::Ignored(SK::ConstDeclaration),
        }
    }

    fn begin_constant_declarator(&mut self) {
        self.start_accumulating_const_refs();
    }

    fn make_constant_declarator(
        &mut self,
        name: Self::Output,
        initializer: Self::Output,
    ) -> Self::Output {
        // The "X=1" part of either a member const "class C {const int X=1;}" or a top-level
        // const "const int X=1;" Note: the the declarator itself doesn't yet know whether a
        // type was provided by the user; that's only known in the parent, make_const_declaration
        let refs = self.stop_accumulating_const_refs();
        if name.is_ignored() {
            Node::Ignored(SK::ConstantDeclarator)
        } else {
            Node::ConstInitializer(Box::new((name, initializer, refs)))
        }
    }

    fn make_namespace_declaration(
        &mut self,
        _name: Self::Output,
        body: Self::Output,
    ) -> Self::Output {
        if matches!(&body, Node::List(..)) {
            let this = Rc::make_mut(&mut self.state);
            Rc::make_mut(&mut this.namespace_builder).pop_namespace();
        }
        body
    }

    fn make_namespace_declaration_header(
        &mut self,
        _keyword: Self::Output,
        name: Self::Output,
    ) -> Self::Output {
        let name = self.expect_name(&name).map(|Id(_, name)| name);
        // if this is header of semicolon-style (one with NamespaceEmptyBody) namespace,
        // we should pop the previous namespace first, but we don't have the body yet.
        // We'll fix it retroactively in make_namespace_empty_body
        let this = Rc::make_mut(&mut self.state);
        Rc::make_mut(&mut this.namespace_builder).push_namespace(name.map(Cow::Owned));
        Node::Ignored(SK::NamespaceDeclarationHeader)
    }

    fn make_namespace_body(
        &mut self,
        _left_brace: Self::Output,
        declarations: Self::Output,
        _right_brace: Self::Output,
    ) -> Self::Output {
        declarations
    }

    fn make_namespace_empty_body(&mut self, _semicolon: Self::Output) -> Self::Output {
        let this = Rc::make_mut(&mut self.state);
        Rc::make_mut(&mut this.namespace_builder).pop_previous_namespace();
        Node::Ignored(SK::NamespaceEmptyBody)
    }

    fn make_namespace_use_declaration(
        &mut self,
        _keyword: Self::Output,
        namespace_use_kind: Self::Output,
        clauses: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        if let Some(import_kind) = Self::namespace_use_kind(&namespace_use_kind) {
            for clause in clauses.into_iter() {
                if let Node::NamespaceUseClause(box nuc) = clause {
                    let this = Rc::make_mut(&mut self.state);
                    Rc::make_mut(&mut this.namespace_builder).add_import(
                        import_kind,
                        &nuc.id.1,
                        nuc.as_.as_deref(),
                    );
                }
            }
        }
        Node::Ignored(SK::NamespaceUseDeclaration)
    }

    fn make_namespace_group_use_declaration(
        &mut self,
        _keyword: Self::Output,
        _kind: Self::Output,
        prefix: Self::Output,
        _left_brace: Self::Output,
        clauses: Self::Output,
        _right_brace: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        let Id(_, prefix) = match self.expect_name(&prefix) {
            Some(id) => id,
            None => return Node::Ignored(SK::NamespaceGroupUseDeclaration),
        };
        for clause in clauses.into_iter() {
            if let Node::NamespaceUseClause(box nuc) = clause {
                let this = Rc::make_mut(&mut self.state);
                Rc::make_mut(&mut this.namespace_builder).add_import(
                    nuc.kind,
                    &format!("{}{}", prefix, nuc.id.1),
                    nuc.as_.as_deref(),
                );
            }
        }
        Node::Ignored(SK::NamespaceGroupUseDeclaration)
    }

    fn make_namespace_use_clause(
        &mut self,
        clause_kind: Self::Output,
        name: Self::Output,
        as_: Self::Output,
        aliased_name: Self::Output,
    ) -> Self::Output {
        let id = match self.expect_name(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::NamespaceUseClause),
        };
        let as_ = if as_.is_token(TokenKind::As) {
            match aliased_name.as_id() {
                Some(name) => Some(name.1),
                None => return Node::Ignored(SK::NamespaceUseClause),
            }
        } else {
            None
        };
        if let Some(kind) = Self::namespace_use_kind(&clause_kind) {
            Node::NamespaceUseClause(Box::new(NamespaceUseClause { kind, id, as_ }))
        } else {
            Node::Ignored(SK::NamespaceUseClause)
        }
    }

    fn make_where_clause(
        &mut self,
        _: Self::Output,
        where_constraints: Self::Output,
    ) -> Self::Output {
        where_constraints
    }

    fn make_where_constraint(
        &mut self,
        left_type: Self::Output,
        operator: Self::Output,
        right_type: Self::Output,
    ) -> Self::Output {
        Node::WhereConstraint(Box::new(WhereConstraint(
            self.node_to_ty(left_type).unwrap_or_else(tany),
            match operator.token_kind() {
                Some(TokenKind::Equal) => ConstraintKind::ConstraintEq,
                Some(TokenKind::Super) => ConstraintKind::ConstraintSuper,
                _ => ConstraintKind::ConstraintAs,
            },
            self.node_to_ty(right_type).unwrap_or_else(tany),
        )))
    }

    fn make_classish_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        xhp_keyword: Self::Output,
        class_keyword: Self::Output,
        name: Self::Output,
        tparams: Self::Output,
        _extends_keyword: Self::Output,
        extends: Self::Output,
        _implements_keyword: Self::Output,
        implements: Self::Output,
        body: Self::Output,
    ) -> Self::Output {
        let raw_name = match self.expect_name(&name) {
            Some(Id(_, name)) => name,
            None => return Node::Ignored(SK::ClassishDeclaration),
        };
        let Id(pos, name) = match self.elaborate_defined_id(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::ClassishDeclaration),
        };
        let is_xhp = raw_name.starts_with(':') || xhp_keyword.is_present();

        let mut final_ = false;
        let mut abstract_ = false;
        let mut internal = false;

        for modifier in modifiers.iter() {
            match modifier.token_kind() {
                Some(TokenKind::Abstract) => {
                    abstract_ = true;
                }
                Some(TokenKind::Final) => final_ = true,
                Some(TokenKind::Internal) => internal = true,
                _ => {}
            }
        }
        let class_kind = match class_keyword.token_kind() {
            Some(TokenKind::Interface) => ClassishKind::Cinterface,
            Some(TokenKind::Trait) => ClassishKind::Ctrait,
            _ => ClassishKind::Cclass(if abstract_ {
                Abstraction::Abstract
            } else {
                Abstraction::Concrete
            }),
        };

        let body = match body {
            Node::ClassishBody(body) => body,
            _ => return Node::Ignored(SK::ClassishDeclaration),
        };

        let mut uses_len = 0;
        let mut xhp_attr_uses_len = 0;
        let mut xhp_enum_values = SMap::new();
        let mut xhp_marked_empty = false;
        let mut req_extends_len = 0;
        let mut req_implements_len = 0;
        let mut req_constraints_len = 0;
        let mut consts_len = 0;
        let mut typeconsts_len = 0;
        let mut props_len = 0;
        let mut sprops_len = 0;
        let mut static_methods_len = 0;
        let mut methods_len = 0;

        let mut user_attributes_len = 0;
        for attribute in attributes.iter() {
            match *attribute {
                Node::Attribute(_) => user_attributes_len += 1,
                _ => {}
            }
        }

        for element in body.iter() {
            match element {
                Node::TraitUse(names) => uses_len += names.len(),
                Node::XhpClassAttributeDeclaration(box XhpClassAttributeDeclarationNode {
                    xhp_attr_decls,
                    xhp_attr_uses_decls,
                    xhp_attr_enum_values,
                }) => {
                    props_len += xhp_attr_decls.len();
                    xhp_attr_uses_len += xhp_attr_uses_decls.len();

                    for (name, values) in xhp_attr_enum_values {
                        xhp_enum_values.insert(name.clone(), values.clone());
                    }
                }
                Node::XhpChildrenDeclaration(XhpChildrenKind::Empty) => {
                    xhp_marked_empty = true;
                }
                Node::TypeConstant(..) => typeconsts_len += 1,
                Node::RequireClause(require) => match require.require_type.token_kind() {
                    Some(TokenKind::Extends) => req_extends_len += 1,
                    Some(TokenKind::Implements) => req_implements_len += 1,
                    Some(TokenKind::Class) => req_constraints_len += 1,
                    _ => {}
                },
                Node::RequireClauseConstraint(_) => {
                    req_constraints_len += 1;
                }
                Node::List(nodes) if matches!(nodes.first(), Some(Node::Const(..))) => {
                    consts_len += nodes.len()
                }
                Node::Property(PropertyNode { decls, is_static }) => {
                    if *is_static {
                        sprops_len += decls.len()
                    } else {
                        props_len += decls.len()
                    }
                }
                Node::Constructor(box ConstructorNode { properties, .. }) => {
                    props_len += properties.len()
                }
                Node::Method(box MethodNode { is_static, .. }) => {
                    if *is_static {
                        static_methods_len += 1
                    } else {
                        methods_len += 1
                    }
                }
                _ => {}
            }
        }

        let mut constructor = None;

        let mut uses = Vec::with_capacity(uses_len);
        let mut xhp_attr_uses = Vec::with_capacity(xhp_attr_uses_len);
        let mut req_extends = Vec::with_capacity(req_extends_len);
        let mut req_implements = Vec::with_capacity(req_implements_len);
        let mut req_constraints = Vec::with_capacity(req_constraints_len);
        let mut consts = Vec::with_capacity(consts_len);
        let mut typeconsts = Vec::with_capacity(typeconsts_len);
        let mut props = Vec::with_capacity(props_len);
        let mut sprops = Vec::with_capacity(sprops_len);
        let mut static_methods = Vec::with_capacity(static_methods_len);
        let mut methods = Vec::with_capacity(methods_len);

        let mut user_attributes = Vec::with_capacity(user_attributes_len);
        let mut docs_url = None;
        let class_attributes = self.to_attributes(&attributes);
        for attribute in attributes.into_iter() {
            match attribute {
                Node::Attribute(attr) => {
                    if attr.name.1 == "__Docs" {
                        if let Some((_, bstr)) = &attr.string_literal_param {
                            docs_url = Some(Self::str_from_utf8(bstr).into_owned());
                        }
                    }
                    user_attributes.push(self.user_attribute_to_decl(*attr));
                }
                _ => {}
            }
        }
        // Match ordering of attributes produced by the OCaml decl parser (even
        // though it's the reverse of the syntactic ordering).
        user_attributes.reverse();

        // xhp props go after regular props, regardless of their order in file
        let mut xhp_props = vec![];

        for element in body {
            match element {
                Node::TraitUse(names) => {
                    uses.extend(names.into_iter().filter_map(|name| self.node_to_ty(name)))
                }
                Node::XhpClassAttributeDeclaration(box XhpClassAttributeDeclarationNode {
                    xhp_attr_decls,
                    xhp_attr_uses_decls,
                    ..
                }) => {
                    xhp_props.extend(xhp_attr_decls);
                    xhp_attr_uses.extend(
                        xhp_attr_uses_decls
                            .into_iter()
                            .filter_map(|node| self.node_to_ty(node)),
                    )
                }
                Node::TypeConstant(constant) => typeconsts.push(*constant),
                Node::RequireClause(require) => match require.require_type.token_kind() {
                    Some(TokenKind::Extends) => {
                        req_extends.extend(self.node_to_ty(require.name).into_iter())
                    }
                    Some(TokenKind::Implements) => {
                        req_implements.extend(self.node_to_ty(require.name).into_iter())
                    }
                    Some(TokenKind::Class) => {
                        if let Some(ty) = self.node_to_ty(require.name) {
                            req_constraints.push(DeclConstraintRequirement::DCREqual(ty))
                        }
                    }
                    _ => {}
                },
                Node::RequireClauseConstraint(require) => {
                    if let Some(ty) = self.node_to_ty(require.name) {
                        req_constraints.push(DeclConstraintRequirement::DCRSubtype(ty))
                    }
                }
                Node::List(nodes) if matches!(nodes.first(), Some(Node::Const(..))) => {
                    consts.extend(nodes.into_iter().filter_map(|node| match node {
                        Node::Const(box decl) => Some(decl),
                        _ => None,
                    }));
                }
                Node::Property(PropertyNode { decls, is_static }) => {
                    if is_static {
                        sprops.extend(decls);
                    } else {
                        props.extend(decls);
                    }
                }
                Node::Constructor(box ConstructorNode { method, properties }) => {
                    // Annoyingly, the <<__SupportDynamicType>> annotation on a
                    // class implicitly changes the decls of every constructor inside
                    // it, so we have to reallocate them here.
                    let method = self.possibly_make_supportdyn_method(&class_attributes, method);
                    constructor = Some(method);
                    for property in properties {
                        props.push(property)
                    }
                }
                Node::Method(box MethodNode { method, is_static }) => {
                    // Annoyingly, the <<__SupportDynamicType>> annotation on a
                    // class implicitly changes the decls of every method inside
                    // it, so we have to reallocate them here.
                    let method = self.possibly_make_supportdyn_method(&class_attributes, method);
                    if is_static {
                        static_methods.push(method);
                    } else {
                        methods.push(method);
                    }
                }
                _ => {} // It's not our job to report errors here.
            }
        }

        props.extend(xhp_props);

        let extends = extends
            .into_iter()
            .filter_map(|node| self.node_to_ty(node))
            .collect();
        let implements = implements
            .into_iter()
            .filter_map(|node| self.node_to_ty(node))
            .collect();
        let support_dynamic_type = (self.implicit_sdt()
            && !class_attributes.no_support_dynamic_type)
            || class_attributes.support_dynamic_type;
        // Pop the type params stack only after creating all inner types.
        let tparams = self.pop_type_params(tparams);
        let module = self.module.clone();

        if self.opts.ignore_string_methods && name == naming_special_names::typehints::HH_STRING {
            methods = vec![];
            static_methods = vec![];
        }
        let cls = shallow_decl_defs::ShallowClass {
            mode: self.file_mode,
            final_,
            abstract_,
            is_xhp,
            has_xhp_keyword: xhp_keyword.is_token(TokenKind::XHP),
            kind: class_kind,
            module,
            internal,
            name: (pos, name.clone()),
            tparams,
            extends,
            uses,
            xhp_attr_uses,
            xhp_enum_values,
            xhp_marked_empty,
            req_extends,
            req_implements,
            req_constraints,
            implements,
            support_dynamic_type,
            consts,
            typeconsts,
            props,
            sprops,
            constructor,
            static_methods,
            methods,
            user_attributes,
            enum_type: None,
            docs_url,
            package: self.package.clone(),
        };
        let this = Rc::make_mut(&mut self.state);
        this.class_decl(name, cls)
    }

    fn make_property_declaration(
        &mut self,
        attrs: Self::Output,
        modifiers: Self::Output,
        hint: Self::Output,
        declarators: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        let modifiers = read_member_modifiers(modifiers.iter());
        let declarators = declarators
            .into_iter()
            .filter_map(|declarator| match declarator {
                Node::ListItem(name, initializer) => {
                    let attributes = self.to_attributes(&attrs);
                    let Id(pos, name) = name.as_variable()?;
                    let name = if modifiers.is_static {
                        &name
                    } else {
                        strip_dollar_prefix(&name)
                    };
                    let ty = self.node_to_non_ret_ty(hint.clone());
                    let ty = ty.unwrap_or_else(|| self.tany_with_pos(pos.clone()));
                    let ty = if self.opts.interpret_soft_types_as_like_types {
                        if attributes.soft {
                            Ty(
                                Reason::FromWitnessDecl(WitnessDecl::Hint(self.get_pos(&hint))),
                                Box::new(Ty_::Tlike(ty)),
                            )
                        } else {
                            ty
                        }
                    } else {
                        ty
                    };
                    let needs_init = if self.file_mode == Mode::Mhhi {
                        false
                    } else {
                        initializer.is_ignored()
                    };
                    let mut flags = PropFlags::empty();
                    flags.set(PropFlags::CONST, attributes.const_);
                    flags.set(PropFlags::LATEINIT, attributes.late_init);
                    flags.set(PropFlags::LSB, attributes.lsb);
                    flags.set(PropFlags::NEEDS_INIT, needs_init);
                    flags.set(PropFlags::ABSTRACT, modifiers.is_abstract);
                    flags.set(PropFlags::READONLY, modifiers.is_readonly);
                    flags.set(PropFlags::PHP_STD_LIB, attributes.php_std_lib);
                    flags.set(
                        PropFlags::SAFE_GLOBAL_VARIABLE,
                        attributes.safe_global_variable,
                    );
                    flags.set(PropFlags::NO_AUTO_LIKES, attributes.no_auto_likes);
                    flags.set(
                        PropFlags::TESTS_BYPASS_VISIBILITY,
                        attributes.tests_bypass_visibility,
                    );
                    Some(ShallowProp {
                        xhp_attr: None,
                        name: (pos, name.into()),
                        type_: ty,
                        visibility: modifiers.visibility,
                        flags,
                    })
                }
                _ => None,
            })
            .collect();
        Node::Property(PropertyNode {
            decls: declarators,
            is_static: modifiers.is_static,
        })
    }

    fn make_xhp_children_declaration(
        &mut self,
        _keyword: Self::Output,
        expression: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        match expression {
            Node::IgnoredToken(token) if token.kind() == TokenKind::Empty => {
                Node::XhpChildrenDeclaration(XhpChildrenKind::Empty)
            }
            _ => Node::XhpChildrenDeclaration(XhpChildrenKind::Other),
        }
    }

    fn make_xhp_class_attribute_declaration(
        &mut self,
        _keyword: Self::Output,
        attributes: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        let mut xhp_attr_enum_values = Vec::new();
        let mut xhp_attr_uses_decls = Vec::new();
        let xhp_attr_decls = attributes
            .into_iter()
            .filter_map(|node| {
                let node = match node {
                    Node::XhpAttributeUse(name) => {
                        xhp_attr_uses_decls.push(*name);
                        return None;
                    }
                    Node::XhpClassAttribute(box x) => x,
                    _ => return None,
                };
                let Id(pos, name) = node.name;
                let name = format!(":{}", name);

                let (like, type_, enum_values) = match node.hint {
                    Node::XhpEnumTy(box (like, ty, values)) => (like, ty, Some(values)),
                    _ => (
                        None,
                        self.node_to_ty(node.hint)
                            .unwrap_or_else(|| self.tany_with_pos(pos.clone())),
                        None,
                    ),
                };
                if let Some(enum_values) = enum_values {
                    xhp_attr_enum_values.push((name.clone(), enum_values));
                };

                let type_ = if node.nullable && node.tag.is_none() {
                    match type_ {
                        // already nullable
                        Ty(_, box Ty_::Toption(_)) | Ty(_, box Ty_::Tmixed) => type_,
                        // make nullable
                        _ => Ty(
                            Reason::FromWitnessDecl(WitnessDecl::Hint(type_.get_pos()?.clone())),
                            Box::new(Ty_::Toption(type_)),
                        ),
                    }
                } else {
                    type_
                };
                let type_ = match like {
                    Some(p) => Ty(
                        Reason::FromWitnessDecl(WitnessDecl::Hint(p)),
                        Box::new(Ty_::Tlike(type_)),
                    ),
                    None => type_,
                };

                let mut flags = PropFlags::empty();
                flags.set(PropFlags::NEEDS_INIT, node.needs_init);
                Some(ShallowProp {
                    name: (pos, name),
                    visibility: aast::Visibility::Public,
                    type_,
                    xhp_attr: Some(xhp_attribute::XhpAttribute {
                        tag: node.tag,
                        has_default: !node.needs_init,
                    }),
                    flags,
                })
            })
            .collect();

        Node::XhpClassAttributeDeclaration(Box::new(XhpClassAttributeDeclarationNode {
            xhp_attr_enum_values,
            xhp_attr_decls,
            xhp_attr_uses_decls,
        }))
    }

    /// Handle XHP attribute enum declarations.
    ///
    ///   class :foo implements XHPChild {
    ///     attribute
    ///       enum {'big', 'small'} size; // this line
    ///   }
    fn make_xhp_enum_type(
        &mut self,
        like: Self::Output,
        enum_keyword: Self::Output,
        _left_brace: Self::Output,
        xhp_enum_values: Self::Output,
        right_brace: Self::Output,
    ) -> Self::Output {
        // Infer the type hint from the first value.
        // TODO: T88207956 consider all the values.
        let ty = xhp_enum_values
            .iter()
            .next()
            .and_then(|node| self.node_to_ty(node.clone()))
            .map(|node_ty| {
                let pos = self.merge_positions(&enum_keyword, &right_brace);
                let ty_ = node_ty.1;
                Ty(Reason::FromWitnessDecl(WitnessDecl::Hint(pos)), ty_)
            });
        let mut values = Vec::new();
        for node in xhp_enum_values.into_iter() {
            // XHP enum values may only be string or int literals.
            match node {
                Node::IntLiteral(s, _) => {
                    let i = s.parse::<isize>().unwrap_or(0);
                    values.push(XhpEnumValue::XEVInt(i));
                }
                Node::StringLiteral(s, _) => {
                    let s = String::from_utf8_lossy(&s);
                    values.push(XhpEnumValue::XEVString(s.into_owned()));
                }
                _ => {}
            };
        }

        match ty {
            Some(ty) => Node::XhpEnumTy(Box::new((self.get_pos_opt(&like), ty, values))),
            None => Node::Ignored(SK::XHPEnumType),
        }
    }

    fn make_xhp_class_attribute(
        &mut self,
        type_: Self::Output,
        name: Self::Output,
        initializer: Self::Output,
        tag: Self::Output,
    ) -> Self::Output {
        let name = match name.as_id() {
            Some(name) => name,
            None => return Node::Ignored(SK::XHPClassAttribute),
        };
        Node::XhpClassAttribute(Box::new(XhpClassAttributeNode {
            name,
            hint: type_,
            needs_init: !initializer.is_present(),
            tag: match tag.token_kind() {
                Some(TokenKind::Required) => Some(xhp_attribute::Tag::Required),
                Some(TokenKind::Lateinit) => Some(xhp_attribute::Tag::LateInit),
                _ => None,
            },
            nullable: initializer.is_token(TokenKind::NullLiteral) || !initializer.is_present(),
        }))
    }

    fn make_xhp_simple_class_attribute(&mut self, name: Self::Output) -> Self::Output {
        Node::XhpAttributeUse(Box::new(name))
    }

    fn make_property_declarator(
        &mut self,
        name: Self::Output,
        initializer: Self::Output,
    ) -> Self::Output {
        Node::ListItem(Box::new(name), Box::new(initializer))
    }

    fn make_methodish_declaration(
        &mut self,
        attrs: Self::Output,
        header: Self::Output,
        body: Self::Output,
        closer: Self::Output,
    ) -> Self::Output {
        let header = match header {
            Node::FunctionHeader(box header) => header,
            _ => return Node::Ignored(SK::MethodishDeclaration),
        };
        // If we don't have a body, use the closing token. A closing token of
        // '}' indicates a regular function, while a closing token of ';'
        // indicates an abstract function.
        let body = if body.is_ignored() { closer } else { body };
        let modifiers = read_member_modifiers(header.modifiers.iter());
        let is_constructor = header.name.is_token(TokenKind::Construct);
        let is_method = true;
        let attributes = self.to_attributes(&attrs);
        let (id, ty, properties) = match self.function_to_ty(is_method, &attrs, header, body) {
            Some(tuple) => tuple,
            None => return Node::Ignored(SK::MethodishDeclaration),
        };
        let deprecated = attributes
            .deprecated
            .map(|msg| format!("The method {} is deprecated: {}", id.1, msg));
        let mut flags = MethodFlags::empty();
        flags.set(
            MethodFlags::ABSTRACT,
            self.in_interface() || modifiers.is_abstract,
        );
        flags.set(MethodFlags::FINAL, modifiers.is_final);
        flags.set(MethodFlags::OVERRIDE, attributes.override_);
        flags.set(
            MethodFlags::DYNAMICALLYCALLABLE,
            attributes.dynamically_callable,
        );
        flags.set(MethodFlags::PHP_STD_LIB, attributes.php_std_lib);
        flags.set(
            MethodFlags::SUPPORT_DYNAMIC_TYPE,
            attributes.support_dynamic_type,
        );
        flags.set(MethodFlags::NO_AUTO_LIKES, attributes.no_auto_likes);
        flags.set(MethodFlags::NEEDS_CONCRETE, attributes.needs_concrete);
        flags.set(
            MethodFlags::TESTS_BYPASS_VISIBILITY,
            attributes.tests_bypass_visibility,
        );

        // Parse the user attributes
        // in facts-mode all attributes are saved, otherwise only __NoAutoDynamic/__NoAutoLikes is
        let user_attributes = attrs
            .into_iter()
            .rev()
            .filter_map(|attribute| {
                if let Node::Attribute(attr) = attribute {
                    if self.keep_user_attribute(&attr) {
                        Some(self.user_attribute_to_decl(*attr))
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .collect();

        let method = ShallowMethod {
            name: id,
            type_: ty,
            visibility: modifiers.visibility,
            deprecated,
            flags,
            attributes: user_attributes,
            package_requirement: attributes
                .require_package
                .unwrap_or(PackageRequirement::RPNormal),
        };
        if !self.inside_no_auto_dynamic_class {
            let this = Rc::make_mut(&mut self.state);
            this.under_no_auto_dynamic = false;
            this.under_no_auto_likes = false;
        }
        if is_constructor {
            Node::Constructor(Box::new(ConstructorNode { method, properties }))
        } else {
            Node::Method(Box::new(MethodNode {
                method,
                is_static: modifiers.is_static,
            }))
        }
    }

    fn make_classish_body(
        &mut self,
        _left_brace: Self::Output,
        elements: Self::Output,
        _right_brace: Self::Output,
    ) -> Self::Output {
        Node::ClassishBody(elements.into_iter().collect())
    }

    fn make_enum_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        _keyword: Self::Output,
        name: Self::Output,
        _colon: Self::Output,
        extends: Self::Output,
        constraint: Self::Output,
        _left_brace: Self::Output,
        use_clauses: Self::Output,
        enumerators: Self::Output,
        _right_brace: Self::Output,
    ) -> Self::Output {
        let id = match self.elaborate_defined_id(&name) {
            Some(id) => id,
            None => return Node::Ignored(SK::EnumDeclaration),
        };
        let hint = match self.node_to_ty(extends) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::EnumDeclaration),
        };
        let extends = match self.node_to_ty(self.make_apply(
            (
                self.get_pos(&name),
                naming_special_names::classes::HH_BUILTIN_ENUM.into(),
            ),
            name,
            Pos::NONE,
        )) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::EnumDeclaration),
        };
        let internal = modifiers.iter().any(|m| {
            m.as_visibility() == Some(aast::Visibility::Internal)
                || m.as_visibility() == Some(aast::Visibility::ProtectedInternal)
        });
        let key = id.1.clone();
        let consts = enumerators
            .into_iter()
            .filter_map(|node| match node {
                Node::Const(box c) => Some(c),
                _ => None,
            })
            .collect();
        let parsed_attributes = self.to_attributes(&attributes);
        let mut user_attributes = Vec::with_capacity(attributes.len());
        let mut docs_url = None;
        for attribute in attributes.into_iter() {
            match attribute {
                Node::Attribute(attr) => {
                    if attr.name.1 == "__Docs" {
                        if let Some((_, bstr)) = &attr.string_literal_param {
                            docs_url = Some(Self::str_from_utf8(bstr).into_owned());
                        }
                    }
                    user_attributes.push(self.user_attribute_to_decl(*attr));
                }
                _ => {}
            }
        }
        // Match ordering of attributes produced by the OCaml decl parser (even
        // though it's the reverse of the syntactic ordering).
        user_attributes.reverse();

        let constraint = match constraint {
            Node::TypeConstraint(_kind, box ty) => self.node_to_ty(ty),
            _ => None,
        };

        let mut includes_len = 0;
        for element in use_clauses.iter() {
            match element {
                Node::EnumUse(names) => includes_len += names.len(),
                _ => {}
            }
        }
        let mut includes = Vec::with_capacity(includes_len);
        for element in use_clauses.into_iter() {
            match element {
                Node::EnumUse(names) => {
                    includes.extend(names.into_iter().filter_map(|name| self.node_to_ty(name)))
                }
                _ => {}
            }
        }

        let cls = shallow_decl_defs::ShallowClass {
            mode: self.file_mode,
            final_: false,
            abstract_: false,
            is_xhp: false,
            has_xhp_keyword: false,
            kind: ClassishKind::Cenum,
            module: self.module.clone(),
            internal,
            name: id.into(),
            tparams: vec![],
            extends: vec![extends],
            uses: vec![],
            xhp_attr_uses: vec![],
            xhp_enum_values: SMap::new(),
            xhp_marked_empty: false,
            req_extends: vec![],
            req_implements: vec![],
            req_constraints: vec![],
            implements: vec![],
            support_dynamic_type: parsed_attributes.support_dynamic_type,
            consts,
            typeconsts: vec![],
            props: vec![],
            sprops: vec![],
            constructor: None,
            static_methods: vec![],
            methods: vec![],
            user_attributes,
            enum_type: Some(EnumType {
                base: hint,
                constraint,
                includes,
            }),
            docs_url,
            package: self.package.clone(),
        };
        let this = Rc::make_mut(&mut self.state);
        this.class_decl(key, cls)
    }

    fn make_enum_use(
        &mut self,
        _keyword: Self::Output,
        names: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        Node::EnumUse(Box::new(names))
    }

    fn begin_enumerator(&mut self) {
        self.start_accumulating_const_refs();
    }

    fn make_enumerator(
        &mut self,
        name: Self::Output,
        _equal: Self::Output,
        value: Self::Output,
        semicolon: Self::Output,
    ) -> Self::Output {
        let refs = self.stop_accumulating_const_refs();
        let id = match self.expect_name(&name) {
            Some(id) => id,
            None => return Node::Ignored(SyntaxKind::Enumerator),
        };
        // Inline the "no value" case as CVAbsent (no option wrapper), and
        // collapse `MEMBER = 'MEMBER'` to the nullary CVLabel (no stored string).
        let const_value = if self.opts.include_enum_member_values {
            match self.const_value(&value) {
                Some(ConstValue::CVString(s)) if s == id.1 => ConstValue::CVLabel,
                Some(v) => v,
                None => ConstValue::CVAbsent,
            }
        } else {
            ConstValue::CVAbsent
        };
        let v = if self.opts.include_assignment_values {
            Some(self.node_to_str(value.clone(), &semicolon))
        } else {
            None
        };

        Node::Const(Box::new(ShallowClassConst {
            abstract_: ClassConstKind::CCConcrete,
            type_: self
                .infer_const(name, value)
                .unwrap_or_else(|| self.tany_with_pos(id.0.clone())),
            value: v,
            is_shape_key: false,
            const_value,
            name: id.into(),
            refs,
        }))
    }

    fn make_enum_class_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        _enum_keyword: Self::Output,
        _class_keyword: Self::Output,
        name: Self::Output,
        _colon: Self::Output,
        base: Self::Output,
        _extends_keyword: Self::Output,
        extends_list: Self::Output,
        _left_brace: Self::Output,
        elements: Self::Output,
        _right_brace: Self::Output,
    ) -> Self::Output {
        let name = match self.elaborate_defined_id(&name) {
            Some(name) => name,
            None => return Node::Ignored(SyntaxKind::EnumClassDeclaration),
        };

        let base_pos = self.get_pos(&base);
        let base = self
            .node_to_ty(base)
            .unwrap_or_else(|| self.tany_with_pos(name.0.clone()));

        let mut is_abstract = false;
        let mut is_final = false;
        for modifier in modifiers.iter() {
            match modifier.token_kind() {
                Some(TokenKind::Abstract) => is_abstract = true,
                Some(TokenKind::Final) => is_final = true,
                _ => {}
            }
        }

        let class_kind = if is_abstract {
            ClassishKind::CenumClass(Abstraction::Abstract)
        } else {
            ClassishKind::CenumClass(Abstraction::Concrete)
        };

        let builtin_enum_class_ty = {
            let pos = name.0.clone();
            let enum_class_ty_ = Ty_::Tapply(name.clone().into(), vec![]);
            let enum_class_ty = Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(enum_class_ty_),
            );
            let elt_ty =
                self.make_memberof_type(&base_pos, &pos, enum_class_ty, base.clone(), false);
            let builtin_enum_ty_ = if is_abstract {
                Ty_::Tapply(
                    (
                        pos.clone(),
                        naming_special_names::classes::HH_BUILTIN_ABSTRACT_ENUM_CLASS.into(),
                    ),
                    vec![],
                )
            } else {
                Ty_::Tapply(
                    (
                        pos.clone(),
                        naming_special_names::classes::HH_BUILTIN_ENUM_CLASS.into(),
                    ),
                    vec![elt_ty],
                )
            };
            Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
                Box::new(builtin_enum_ty_),
            )
        };

        let mut consts = Vec::new();
        let mut typeconsts = Vec::new();
        for elem in elements.into_iter() {
            match elem {
                Node::Const(box c) => consts.push(c),
                Node::TypeConstant(box tc) => typeconsts.push(tc),
                _ => {}
            }
        }

        let mut extends = Vec::with_capacity(extends_list.len() + 1);
        extends.push(builtin_enum_class_ty);
        extends.extend(extends_list.into_iter().filter_map(|n| self.node_to_ty(n)));
        let includes = extends[1..].to_vec();

        let mut user_attributes = Vec::with_capacity(attributes.len() + 1);
        let mut docs_url = None;
        let parsed_attributes = self.to_attributes(&attributes);
        for attribute in attributes.into_iter() {
            match attribute {
                Node::Attribute(attr) => {
                    if attr.name.1 == "__Docs" {
                        if let Some((_, bstr)) = &attr.string_literal_param {
                            docs_url = Some(Self::str_from_utf8(bstr).into_owned());
                        }
                    }

                    user_attributes.push(self.user_attribute_to_decl(*attr));
                }
                _ => {}
            }
        }
        let internal = modifiers.iter().any(|m| {
            m.as_visibility() == Some(aast::Visibility::Internal)
                || m.as_visibility() == Some(aast::Visibility::ProtectedInternal)
        });
        user_attributes.push(shallow_decl_defs::UserAttribute {
            name: (name.0.clone(), "__EnumClass".into()),
            params: vec![],
            raw_val: None,
        });
        // Match ordering of attributes produced by the OCaml decl parser (even
        // though it's the reverse of the syntactic ordering).
        user_attributes.reverse();

        let support_dynamic_type = (self.implicit_sdt()
            && !parsed_attributes.no_support_dynamic_type)
            || parsed_attributes.support_dynamic_type;
        let name_string = name.1.clone();

        let cls = shallow_decl_defs::ShallowClass {
            mode: self.file_mode,
            final_: is_final,
            abstract_: is_abstract,
            is_xhp: false,
            has_xhp_keyword: false,
            internal,
            kind: class_kind,
            module: self.module.clone(),
            name: name.into(),
            tparams: vec![],
            extends,
            uses: vec![],
            xhp_attr_uses: vec![],
            xhp_enum_values: SMap::new(),
            req_extends: vec![],
            xhp_marked_empty: false,
            req_implements: vec![],
            req_constraints: vec![],
            implements: vec![],
            support_dynamic_type,
            consts,
            typeconsts,
            props: vec![],
            sprops: vec![],
            constructor: None,
            static_methods: vec![],
            methods: vec![],
            user_attributes,
            enum_type: Some(EnumType {
                base,
                constraint: None,
                includes,
            }),
            docs_url,
            package: self.package.clone(),
        };
        let this = Rc::make_mut(&mut self.state);
        this.class_decl(name_string, cls)
    }

    fn begin_enum_class_enumerator(&mut self) {
        self.start_accumulating_const_refs();
    }

    fn make_enum_class_enumerator(
        &mut self,
        modifiers: Self::Output,
        type_: Self::Output,
        name: Self::Output,
        initializer: Self::Output,
        semicolon: Self::Output,
    ) -> Self::Output {
        let refs = self.stop_accumulating_const_refs();
        let (pos, name) = match self.expect_name(&name) {
            Some(Id(pos, name)) => (pos, name),
            None => return Node::Ignored(SyntaxKind::EnumClassEnumerator),
        };
        let has_abstract_keyword = modifiers
            .iter()
            .any(|node| node.is_token(TokenKind::Abstract));
        let abstract_ = if has_abstract_keyword {
            /* default values not allowed atm */
            ClassConstKind::CCAbstract(false)
        } else {
            ClassConstKind::CCConcrete
        };
        let type_pos = self.get_pos(&type_);
        let type_ = self
            .node_to_ty(type_)
            .unwrap_or_else(|| self.tany_with_pos(pos.clone()));
        let class_name = match self.get_current_classish_name() {
            Some((name, _)) => name,
            None => return Node::Ignored(SyntaxKind::EnumClassEnumerator),
        };
        let enum_class_ty_ = Ty_::Tapply((pos.clone(), class_name.into()), vec![]);
        let enum_class_ty = Ty(
            Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
            Box::new(enum_class_ty_),
        );
        let type_ = self.make_memberof_type(&type_pos, &pos, enum_class_ty, type_, true);

        Node::Const(Box::new(ShallowClassConst {
            abstract_,
            name: (pos, name),
            type_,
            refs,
            value: if self.opts.include_assignment_values {
                Some(self.node_to_str(initializer, &semicolon))
            } else {
                None
            },
            is_shape_key: false,
            const_value: ConstValue::CVAbsent,
        }))
    }

    fn make_tuple_type_specifier(
        &mut self,
        left_paren: Self::Output,
        tys: Self::Output,
        right_paren: Self::Output,
    ) -> Self::Output {
        // We don't need to include the tys list in this position merging
        // because by definition it's already contained by the two brackets.
        let pos = self.merge_positions(&left_paren, &right_paren);
        // Lowerer will check that required precede optional precede at most one variadic element
        let mut required = vec![];
        let mut optional = vec![];
        let mut variadic_opt = None;
        let mut splat_opt = None;
        for node in tys.into_iter() {
            match node {
                Node::TupleComponent(box TupleComponentNode {
                    optional: opt,
                    pre_ellipsis,
                    hint,
                    ellipsis,
                }) => {
                    if let Some(ty) = self.node_to_ty(hint) {
                        if pre_ellipsis {
                            splat_opt = Some(ty)
                        } else if ellipsis {
                            if variadic_opt.is_none() {
                                variadic_opt = Some(ty);
                            }
                        } else if opt {
                            optional.push(ty);
                        } else {
                            required.push(ty);
                        }
                    }
                }
                _ => {}
            }
        }
        let variadic = match variadic_opt {
            None => Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(Ty_::Tunion(vec![])),
            ),
            Some(ty) => ty,
        };
        let extra = match splat_opt {
            None => TupleExtra::Tvariadic(variadic),
            Some(hint) => TupleExtra::Tsplat(hint),
        };
        self.hint_ty(
            pos,
            Ty_::Ttuple(TupleType {
                required,
                optional,
                extra,
            }),
        )
    }

    fn make_tuple_or_union_or_intersection_element_type_specifier(
        &mut self,
        optional: Self::Output,
        pre_ellipsis: Self::Output,
        type_: Self::Output,
        ellipsis: Self::Output,
    ) -> Self::Output {
        Node::TupleComponent(Box::new(TupleComponentNode {
            optional: optional.is_present(),
            pre_ellipsis: pre_ellipsis.is_present(),
            hint: type_,
            ellipsis: ellipsis.is_present(),
        }))
    }

    fn make_tuple_type_explicit_specifier(
        &mut self,
        keyword: Self::Output,
        _left_angle: Self::Output,
        types: Self::Output,
        right_angle: Self::Output,
    ) -> Self::Output {
        let id = (self.get_pos(&keyword), "\\tuple".into());
        // This is an error--tuple syntax is (A, B), not tuple<A, B>.
        // OCaml decl makes a Tapply rather than a Ttuple here.
        self.make_apply(id, types, self.get_pos(&right_angle))
    }

    fn make_intersection_type_specifier(
        &mut self,
        left_paren: Self::Output,
        tys: Self::Output,
        right_paren: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&left_paren, &right_paren);
        let tys = tys
            .into_iter()
            .filter_map(|x| match x {
                Node::ListItem(box ty, _ampersand) => self.node_to_ty(ty),
                x => self.node_to_ty(x),
            })
            .collect();
        self.hint_ty(pos, Ty_::Tintersection(tys))
    }

    fn make_union_type_specifier(
        &mut self,
        left_paren: Self::Output,
        tys: Self::Output,
        right_paren: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&left_paren, &right_paren);
        let tys = tys
            .into_iter()
            .filter_map(|x| match x {
                Node::ListItem(box ty, _bar) => self.node_to_ty(ty),
                x => self.node_to_ty(x),
            })
            .collect();
        self.hint_ty(pos, Ty_::Tunion(tys))
    }

    fn make_shape_type_specifier(
        &mut self,
        shape: Self::Output,
        _lparen: Self::Output,
        fields_in: Self::Output,
        ellipsis_type: Self::Output,
        open: Self::Output,
        rparen: Self::Output,
    ) -> Self::Output {
        let has_splats = fields_in
            .iter()
            .any(|n| matches!(n, Node::ShapeSplatSpecifier(_)));
        if has_splats {
            let pos = self.merge_positions(&shape, &rparen);
            // The shape's openness: a typed open (`T...`) contributes its type, a
            // bare `...` contributes `mixed`, and a closed shape contributes
            // nothing.
            let open_unknown = match self.node_to_ty(ellipsis_type) {
                Some(ty) => Some(ty),
                None if matches!(open.token_kind(), Some(TokenKind::DotDotDot)) => {
                    Some(self.make_variadic_type(&open))
                }
                None => None,
            };
            // A field-run, a sequence of contiguous fields, carries the openness
            // itself so its own explicit fields keep their declared types i.e.
            // `shape(..., 'y' => string, ...)`
            //  means
            // `shape(..., ...shape('y' => string, ...))`.
            // A closed shape uses `nothing`.
            let field_run_kind = open_unknown.clone().unwrap_or_else(|| {
                Ty(
                    Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                    Box::new(Ty_::Tunion(vec![])),
                )
            });
            let mut elems = Vec::new();
            let mut pending_fields = TShapeMap::new();
            let mut made_field_run = false;
            // Rewrite adjacent fields not interspersed by splats to simple shapes
            for node in fields_in.into_iter() {
                match node {
                    Node::ShapeFieldSpecifier(box ShapeFieldNode { name, type_ }) => {
                        let key = self.make_t_shape_field_name(name);
                        if pending_fields.contains_key(&key) {
                            pending_fields.remove(&key);
                        }
                        pending_fields.insert(key, type_);
                    }
                    Node::ShapeSplatSpecifier(box splat_node) => {
                        if !pending_fields.is_empty() {
                            let simple = self.hint_ty(
                                pos.clone(),
                                Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                                    origin: TypeOrigin::MissingOrigin,
                                    unknown_value: field_run_kind.clone(),
                                    fields: std::mem::take(&mut pending_fields),
                                })),
                            );
                            if let Some(ty) = self.node_to_ty(simple) {
                                elems.push(ty);
                                made_field_run = true;
                            }
                        }
                        // Always contribute a splat element, mirroring
                        // `decl_hint.ml` where `hint` is total. If the inner
                        // hint fails to convert, fall back to a placeholder so
                        // the element isn't silently dropped.
                        let splat_ty = self
                            .node_to_ty(splat_node)
                            .unwrap_or_else(|| self.tany_with_pos(pos.clone()));
                        elems.push(splat_ty);
                    }
                    _ => {}
                }
            }
            if !pending_fields.is_empty() {
                let simple = self.hint_ty(
                    pos.clone(),
                    Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                        origin: TypeOrigin::MissingOrigin,
                        unknown_value: field_run_kind,
                        fields: pending_fields,
                    })),
                );
                if let Some(ty) = self.node_to_ty(simple) {
                    elems.push(ty);
                    made_field_run = true;
                }
            }
            // If the shape is open but no field-run captured the openness (it
            // consists only of splats, e.g. `shape(...Base, ...)`), the trailing
            // `...` / `T...` would otherwise be lost. Represent it as a standalone
            // open-empty element (i.e. `shape(...Base, ...shape(...))`).
            if let Some(unknown_value) = open_unknown {
                if !made_field_run {
                    let open_empty = self.hint_ty(
                        pos.clone(),
                        Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                            origin: TypeOrigin::MissingOrigin,
                            unknown_value,
                            fields: TShapeMap::new(),
                        })),
                    );
                    if let Some(ty) = self.node_to_ty(open_empty) {
                        elems.push(ty);
                    }
                }
            }
            self.hint_ty(
                pos,
                Ty_::Tshape(ShapeType::ShapeSplat(ShapeTypeSplat { ss_elems: elems })),
            )
        } else {
            let mut fields = TShapeMap::new();
            for node in fields_in.into_iter() {
                if let Node::ShapeFieldSpecifier(box ShapeFieldNode { name, type_ }) = node {
                    let key = self.make_t_shape_field_name(name);
                    if fields.contains_key(&key) {
                        fields.remove(&key);
                    }
                    fields.insert(key, type_);
                }
            }
            // If a typed ellipsis was provided (e.g. `string...`), use its type
            // for unknown fields. Otherwise fall back to the bare `...` handling.
            let kind = match self.node_to_ty(ellipsis_type) {
                Some(ty) => ty,
                None => self.make_variadic_type(&open),
            };
            let pos = self.merge_positions(&shape, &rparen);
            let origin = TypeOrigin::MissingOrigin;
            self.hint_ty(
                pos,
                Ty_::Tshape(ShapeType::ShapeSimple(ShapeTypeSimple {
                    origin,
                    unknown_value: kind,
                    fields,
                })),
            )
        }
    }

    fn make_classname_type_specifier(
        &mut self,
        classname: Self::Output,
        _lt: Self::Output,
        targ: Self::Output,
        _trailing_comma: Self::Output,
        gt: Self::Output,
    ) -> Self::Output {
        let id = match classname.as_id() {
            Some(id) => id,
            None => return Node::Ignored(SK::ClassnameTypeSpecifier),
        };
        if gt.is_ignored() {
            self.hint_ty(id.0.clone(), mk_string_ty_(id.0))
        } else {
            self.make_apply(
                (id.0, self.elaborate_raw_id(&id.1).into_owned()),
                targ,
                self.merge_positions(&classname, &gt),
            )
        }
    }

    fn make_class_ptr_type_specifier(
        &mut self,
        kw: Self::Output,
        _lt: Self::Output,
        targ: Self::Output,
        _trailing_comma: Self::Output,
        gt: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&kw, &gt);
        let cls = match (kw.token_kind(), self.node_to_ty(targ)) {
            (Some(TokenKind::Class), Some(ty)) => ty,
            (Some(TokenKind::Enum), Some(ty)) => Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(Ty_::Tapply(
                    (
                        pos.clone(),
                        naming_special_names::classes::HH_BUILTIN_ENUM.to_string(),
                    ),
                    vec![ty],
                )),
            ),
            _ => return Node::Ignored(SK::ClassPtrTypeSpecifier),
        };
        Node::Ty(Box::new(Ty(
            Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
            Box::new(Ty_::TclassPtr(cls)),
        )))
    }

    fn make_scope_resolution_expression(
        &mut self,
        class_name: Self::Output,
        _operator: Self::Output,
        value: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&class_name, &value);
        let Id(class_name_pos, class_name_str) = match self.elaborate_class_id(&class_name) {
            Some(id) => id,
            None => return Node::Ignored(SK::ScopeResolutionExpression),
        };
        let class_id = aast::ClassId(
            (),
            class_name_pos.clone(),
            match class_name {
                Node::Name(name, _) if &name == "self" => aast::ClassId_::CIself,
                _ => aast::ClassId_::CI(Id(class_name_pos, class_name_str)),
            },
        );
        let value_id = match self.expect_name(&value) {
            Some(id) => id,
            None => return Node::Ignored(SK::ScopeResolutionExpression),
        };
        self.accumulate_const_ref(&class_id, &value_id);
        Node::Expr(Box::new(aast::Expr(
            (),
            pos,
            nast::Expr_::ClassConst(Box::new((class_id, (value_id.0, value_id.1)))),
        )))
    }

    fn make_nameof_expression(
        &mut self,
        keyword: Self::Output,
        class_name: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&keyword, &class_name);
        let Id(class_name_pos, class_name_str) = match self.elaborate_class_id(&class_name) {
            Some(id) => id,
            None => return Node::Ignored(SK::NameofExpression),
        };
        let class_id = aast::ClassId(
            (),
            class_name_pos.clone(),
            match class_name {
                Node::Name(name, _) if name == "self" => aast::ClassId_::CIself,
                _ => aast::ClassId_::CI(Id(class_name_pos, class_name_str)),
            },
        );
        Node::Expr(Box::new(aast::Expr(
            (),
            pos,
            nast::Expr_::Nameof(Box::new(class_id)),
        )))
    }

    fn make_field_specifier(
        &mut self,
        question_token: Self::Output,
        name: Self::Output,
        _arrow: Self::Output,
        type_: Self::Output,
    ) -> Self::Output {
        let optional = question_token.is_present();
        let ty = match self.node_to_ty(type_) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::FieldSpecifier),
        };
        let name = match self.make_shape_field_name(name) {
            Some(name) => name,
            None => return Node::Ignored(SK::FieldSpecifier),
        };
        Node::ShapeFieldSpecifier(Box::new(ShapeFieldNode {
            name: ShapeField(name),
            type_: ShapeFieldType { optional, ty },
        }))
    }

    fn make_absent_field_specifier(
        &mut self,
        keyword: Self::Output,
        name: Self::Output,
    ) -> Self::Output {
        // `absent 'x'` is surface sugar for `?'x' => nothing`.
        // Span the keyword and the name so the synthetic `nothing`'s witness
        // position matches the lowerer, which uses the whole field node's span.
        let pos = self.merge_positions(&keyword, &name);
        let name = match self.make_shape_field_name(name) {
            Some(name) => name,
            None => return Node::Ignored(SK::AbsentFieldSpecifier),
        };
        let ty = Ty(
            Reason::FromWitnessDecl(WitnessDecl::Hint(pos)),
            Box::new(Ty_::Tunion(vec![])),
        );
        Node::ShapeFieldSpecifier(Box::new(ShapeFieldNode {
            name: ShapeField(name),
            type_: ShapeFieldType { optional: true, ty },
        }))
    }

    fn make_shape_splat_specifier(
        &mut self,
        _ellipsis: Self::Output,
        type_: Self::Output,
    ) -> Self::Output {
        Node::ShapeSplatSpecifier(Box::new(type_))
    }

    fn make_field_initializer(
        &mut self,
        key: Self::Output,
        _arrow: Self::Output,
        value: Self::Output,
    ) -> Self::Output {
        Node::ListItem(Box::new(key), Box::new(value))
    }

    fn make_varray_type_specifier(
        &mut self,
        varray_keyword: Self::Output,
        _less_than: Self::Output,
        tparam: Self::Output,
        _trailing_comma: Self::Output,
        greater_than: Self::Output,
    ) -> Self::Output {
        let tparam = match self.node_to_ty(tparam) {
            Some(ty) => ty,
            None => self.tany_with_pos(self.get_pos(&varray_keyword)),
        };
        self.hint_ty(
            self.merge_positions(&varray_keyword, &greater_than),
            Ty_::Tapply(
                (
                    self.get_pos(&varray_keyword),
                    naming_special_names::collections::VEC.into(),
                ),
                vec![tparam],
            ),
        )
    }

    fn make_darray_type_specifier(
        &mut self,
        darray: Self::Output,
        _less_than: Self::Output,
        key_type: Self::Output,
        _comma: Self::Output,
        value_type: Self::Output,
        _trailing_comma: Self::Output,
        greater_than: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&darray, &greater_than);
        let key_type = self.node_to_ty(key_type).unwrap_or_else(tany);
        let value_type = self.node_to_ty(value_type).unwrap_or_else(tany);
        self.hint_ty(
            pos,
            Ty_::Tapply(
                (
                    self.get_pos(&darray),
                    naming_special_names::collections::DICT.into(),
                ),
                vec![key_type, value_type],
            ),
        )
    }

    fn make_old_attribute_specification(
        &mut self,
        ltlt: Self::Output,
        attrs: Self::Output,
        gtgt: Self::Output,
    ) -> Self::Output {
        if attrs.contains_marker_attribute("__NoAutoDynamic") {
            let this = Rc::make_mut(&mut self.state);
            this.under_no_auto_dynamic = true;
        }
        if attrs.contains_marker_attribute("__NoAutoLikes") {
            let this = Rc::make_mut(&mut self.state);
            this.under_no_auto_likes = true;
        }
        match attrs {
            Node::List(nodes) => {
                Node::BracketedList(Box::new((self.get_pos(&ltlt), nodes, self.get_pos(&gtgt))))
            }
            _ => Node::Ignored(SK::OldAttributeSpecification),
        }
    }

    fn make_constructor_call(
        &mut self,
        name: Self::Output,
        left_paren: Self::Output,
        args: Self::Output,
        right_paren: Self::Output,
    ) -> Self::Output {
        let unqualified_name = match self.expect_name(&name) {
            Some(name) => name,
            None => return Node::Ignored(SK::ConstructorCall),
        };
        let name = if unqualified_name.1.starts_with("__") {
            unqualified_name
        } else {
            match self.expect_name(&name) {
                Some(name) => self.elaborate_id(name),
                None => return Node::Ignored(SK::ConstructorCall),
            }
        };
        let build_name = |pos, class_name: String| {
            let name = if class_name.starts_with(':') && self.opts.disable_xhp_element_mangling {
                // for facts, allow xhp class consts to be mangled later on
                // even when xhp_element_mangling is disabled
                Id(pos, format!("\\{}", class_name))
            } else {
                self.elaborate_id(Id(pos, class_name))
            };
            Some(AttributeParam::Classname(name))
        };
        let params: Vec<_> = args
            .into_iter()
            .filter_map(|node| match node {
                Node::Expr(box aast::Expr(
                    _,
                    _,
                    aast::Expr_::ClassConst(box (
                        aast::ClassId(_, _, aast::ClassId_::CI(Id(pos, class_name))),
                        (_, pstring),
                    )),
                )) if pstring == "class" => build_name(pos, class_name),
                Node::Expr(box aast::Expr(
                    _,
                    _,
                    aast::Expr_::Nameof(box aast::ClassId(
                        _,
                        _,
                        aast::ClassId_::CI(Id(pos, class_name)),
                    )),
                )) => build_name(pos, class_name),
                Node::EnumClassLabel(label) => Some(AttributeParam::EnumClassLabel(label)),
                Node::Expr(ref e @ box aast::Expr(_, ref pos, _)) => {
                    // Try to parse a sequence of string concatenations
                    let mut acc = BString::new(vec![]);
                    self.fold_string_concat(e, &mut acc)
                        .then(|| AttributeParam::String(pos.clone(), acc))
                }
                Node::StringLiteral(slit, pos) => Some(AttributeParam::String(pos, slit)),
                Node::IntLiteral(ilit, _) => Some(AttributeParam::Int(ilit)),
                _ => None,
            })
            .collect();
        let string_literal_param = params.first().and_then(|p| match p {
            AttributeParam::String(pos, s) => Some((pos.clone(), s.clone())),
            _ => None,
        });
        let raw_val = self.opts.include_assignment_values.then(|| {
            Self::str_from_utf8(self.source_text.source_text().sub(
                self.get_pos(&left_paren).end_offset(),
                self.get_pos(&right_paren).start_offset() - self.get_pos(&left_paren).end_offset(),
            ))
            .trim()
            .to_string()
        });
        Node::Attribute(Box::new(UserAttributeNode {
            name,
            params,
            string_literal_param,
            raw_val,
        }))
    }

    fn make_trait_use(
        &mut self,
        _keyword: Self::Output,
        names: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        Node::TraitUse(Box::new(names))
    }

    fn make_require_clause(
        &mut self,
        _keyword: Self::Output,
        require_type: Self::Output,
        name: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        Node::RequireClause(Box::new(RequireClause { require_type, name }))
    }

    fn make_require_clause_constraint(
        &mut self,
        _keyword: Self::Output,
        _this: Self::Output,
        _as: Self::Output,
        name: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        Node::RequireClauseConstraint(Box::new(RequireClauseConstraint { name }))
    }

    fn make_nullable_type_specifier(
        &mut self,
        question_mark: Self::Output,
        hint: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&question_mark, &hint);
        let ty = match self.node_to_ty(hint) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::NullableTypeSpecifier),
        };
        self.hint_ty(pos, Ty_::Toption(ty))
    }

    fn make_like_type_specifier(
        &mut self,
        tilde: Self::Output,
        hint: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&tilde, &hint);
        let ty = match self.node_to_ty(hint) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::LikeTypeSpecifier),
        };
        self.hint_ty(pos, Ty_::Tlike(ty))
    }

    fn make_closure_type_specifier(
        &mut self,
        outer_left_paren: Self::Output,
        readonly_keyword: Self::Output,
        _function_keyword: Self::Output,
        type_parameters: Self::Output,
        _inner_left_paren: Self::Output,
        parameter_list: Self::Output,
        _inner_right_paren: Self::Output,
        capability: Self::Output,
        _colon: Self::Output,
        readonly_ret: Self::Output,
        return_type: Self::Output,
        outer_right_paren: Self::Output,
    ) -> Self::Output {
        let mut ft_variadic = false;
        let mut ft_named_variadic = false;
        let mut make_param = |fp: FunParamDecl| -> FunParam {
            let mut flags = FunParamFlags::empty();

            let pos = self.get_pos(&fp.hint);
            let mut param_type = self.node_to_ty(fp.hint).unwrap_or_else(tany);
            if let ParamMode::FPinout = fp.kind {
                flags |= FunParamFlags::INOUT;
                // Pessimise type for inout
                param_type = if self.implicit_sdt() && !self.no_auto_likes() {
                    Ty(
                        Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                        Box::new(Ty_::Tlike(param_type)),
                    )
                } else {
                    param_type
                }
            };

            if fp.optional {
                flags |= FunParamFlags::IS_OPTIONAL;
            }
            if fp.readonly {
                flags |= FunParamFlags::READONLY;
            }
            if fp.variadic {
                if fp.named {
                    ft_named_variadic = true;
                } else {
                    ft_variadic = true;
                }
            }
            if fp.splat {
                flags |= FunParamFlags::SPLAT;
            }
            if fp.named {
                flags |= FunParamFlags::NAMED;
            }

            FunParam {
                pos,
                name: fp.name.clone(),
                type_: param_type,
                flags,
                def_value: None, // Not supported for closures
            }
        };

        let params = parameter_list
            .into_iter()
            .filter_map(|node| match node {
                Node::FunParam(box fp) => Some(make_param(fp)),
                _ => None,
            })
            .collect();

        let tparams =
            match type_parameters {
                Node::TypeParameters(tparams) => {
                    // Iterate over the old tparams and add `supportdyn<mixed>` upper bounds
                    // unless there is `__NoAutoBound` attribute
                    if self.implicit_sdt() {
                        tparams
                            .into_iter()
                            .map(|mut tparam| {
                                if !tparam.user_attributes.iter().any(|ua| {
                            ua.name.1 == naming_special_names_rust::user_attributes::NO_AUTO_BOUND
                        }) {
                            let mixed = Ty(
                                Reason::FromWitnessDecl(WitnessDecl::Hint(tparam.name.0.clone())),
                                Box::new(Ty_::Tmixed),
                            );
                            let ub = Ty(
                                Reason::FromWitnessDecl(WitnessDecl::Hint(tparam.name.0.clone())),
                                Box::new(Ty_::Tapply(
                                    (
                                        tparam.name.0.clone(),
                                        naming_special_names_rust::classes::SUPPORT_DYN.to_string(),
                                    ),
                                    vec![mixed],
                                )),
                            );
                            tparam.constraints.push((ConstraintKind::ConstraintAs, ub));
                            tparam
                        } else {
                            tparam
                        }
                            })
                            .collect()
                    } else {
                        tparams
                    }
                }
                _ => vec![],
            };
        let instantiated = tparams.is_empty();

        let ret = match self.node_to_ty(return_type) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::ClosureTypeSpecifier),
        };
        let pos = self.merge_positions(&outer_left_paren, &outer_right_paren);
        let implicit_params = self.as_fun_implicit_params(capability, pos.clone());

        let mut flags = FunTypeFlags::empty();
        if readonly_ret.is_token(TokenKind::Readonly) {
            flags |= FunTypeFlags::RETURNS_READONLY;
        }
        if readonly_keyword.is_token(TokenKind::Readonly) {
            flags |= FunTypeFlags::READONLY_THIS;
        }
        if ft_variadic {
            flags |= FunTypeFlags::VARIADIC
        }
        if ft_named_variadic {
            flags |= FunTypeFlags::NAMED_VARIADIC
        }

        let pess_return_type = if self.implicit_sdt() && !self.no_auto_likes() {
            Ty(
                Reason::FromWitnessDecl(WitnessDecl::Hint(pos.clone())),
                Box::new(Ty_::Tlike(ret)),
            )
        } else {
            ret
        };
        let mut fty = Ty_::Tfun(FunType {
            tparams,
            where_constraints: vec![],
            params,
            implicit_params,
            ret: pess_return_type,
            flags,
            instantiated,
        });

        if self.implicit_sdt() {
            fty = self.make_supportdyn(pos.clone(), fty);
        }
        self.hint_ty(pos, fty)
    }

    fn make_closure_parameter_type_specifier(
        &mut self,
        optional: Self::Output,
        inout: Self::Output,
        named: Self::Output,
        readonly: Self::Output,
        pre_ellipsis: Self::Output,
        hint: Self::Output,
        param_name: Self::Output,
        ellipsis: Self::Output,
    ) -> Self::Output {
        let kind = if inout.is_token(TokenKind::Inout) {
            ParamMode::FPinout
        } else {
            ParamMode::FPnormal
        };
        let pos = self.get_pos(&hint);
        Node::FunParam(Box::new(FunParamDecl {
            attributes: Node::Ignored(SK::Missing),
            visibility: Node::Ignored(SK::Missing),
            kind,
            hint,
            optional: optional.is_token(TokenKind::Optional),
            readonly: readonly.is_token(TokenKind::Readonly),
            named: named.is_token(TokenKind::Named),
            pos,
            name: match &param_name {
                Node::Variable(name_str, _) => Some(name_str.clone()),
                _ => None,
            },
            variadic: ellipsis.is_token(TokenKind::DotDotDot),
            splat: pre_ellipsis.is_token(TokenKind::DotDotDot),
            initializer: Node::Ignored(SK::Missing),
            parameter_end: Node::Ignored(SK::Missing),
        }))
    }

    fn make_type_const_declaration(
        &mut self,
        attributes: Self::Output,
        modifiers: Self::Output,
        _const_keyword: Self::Output,
        _type_keyword: Self::Output,
        name: Self::Output,
        _type_parameters: Self::Output,
        constraints: Self::Output,
        _equal: Self::Output,
        type_: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        let attributes = self.to_attributes(&attributes);
        let has_abstract_keyword = modifiers
            .iter()
            .any(|node| node.is_token(TokenKind::Abstract));
        let reduce_bounds = |mut constraints: Vec<Ty>, f: fn(Vec<Ty>) -> Ty_| {
            if constraints.len() == 1 {
                constraints.pop()
            } else {
                #[allow(clippy::manual_map)]
                // map doesn't allow moving out of borrowed constraints
                match constraints.first() {
                    None => None, // no bounds
                    Some(fst) => Some(Ty(fst.0.clone(), Box::new(f(constraints)))),
                }
            }
        };
        let type_ = self.node_to_ty(type_);
        let kind = if has_abstract_keyword {
            // Abstract type constant in EBNF-like notation:
            //     abstract const type T {as U | super L} [= D];
            let (lower, upper) = self.partition_type_bounds_into_lower_and_upper(constraints);
            Typeconst::TCAbstract(AbstractTypeconst {
                // `as T1 as T2 as ...` == `as (T1 & T2 & ...)`
                as_constraint: reduce_bounds(upper, Ty_::Tintersection),
                // `super T1 super T2 super ...` == `super (T1 | T2 | ...)`
                super_constraint: reduce_bounds(lower, Ty_::Tunion),
                default: type_,
            })
        } else if let Some(tc_type) = type_ {
            // Concrete type constant:
            //     const type T = Z;
            Typeconst::TCConcrete(ConcreteTypeconst { tc_type })
        } else {
            // concrete or type constant requires a value
            return Node::Ignored(SK::TypeConstDeclaration);
        };
        let name = match name.as_id() {
            Some(name) => name,
            None => return Node::Ignored(SK::TypeConstDeclaration),
        };
        Node::TypeConstant(Box::new(ShallowTypeconst {
            name: name.into(),
            kind,
            enforceable: match attributes.enforceable {
                Some(pos) => (pos, true),
                None => (Pos::NONE, false),
            },
            reifiable: attributes.reifiable,
            is_ctx: false,
        }))
    }

    fn make_context_const_declaration(
        &mut self,
        modifiers: Self::Output,
        _const_keyword: Self::Output,
        _ctx_keyword: Self::Output,
        name: Self::Output,
        _type_parameters: Self::Output,
        constraints: Self::Output,
        _equal: Self::Output,
        ctx_list: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        let name = match name.as_id() {
            Some(name) => name,
            None => return Node::Ignored(SK::TypeConstDeclaration),
        };
        let has_abstract_keyword = modifiers
            .iter()
            .any(|node| node.is_token(TokenKind::Abstract));
        let context = self.node_to_ty(ctx_list);

        // note: lowerer ensures that there's at most 1 constraint of each kind
        let mut as_constraint = None;
        let mut super_constraint = None;
        for c in constraints.into_iter() {
            if let Node::ContextConstraint(kind, box hint) = c {
                let ty = self.node_to_ty(hint);
                match kind {
                    ConstraintKind::ConstraintSuper => super_constraint = ty,
                    ConstraintKind::ConstraintAs => as_constraint = ty,
                    _ => {}
                }
            }
        }
        let kind = if has_abstract_keyword {
            Typeconst::TCAbstract(AbstractTypeconst {
                as_constraint,
                super_constraint,
                default: context,
            })
        } else if let Some(tc_type) = context {
            Typeconst::TCConcrete(ConcreteTypeconst { tc_type })
        } else {
            /* Concrete type const must have a value */
            return Node::Ignored(SK::TypeConstDeclaration);
        };
        Node::TypeConstant(Box::new(ShallowTypeconst {
            name: name.into(),
            kind,
            enforceable: (Pos::NONE, false),
            reifiable: None,
            is_ctx: true,
        }))
    }

    fn make_decorated_expression(
        &mut self,
        decorator: Self::Output,
        expr: Self::Output,
    ) -> Self::Output {
        Node::ListItem(Box::new(decorator), Box::new(expr))
    }

    fn make_type_constant(
        &mut self,
        ty: Self::Output,
        _coloncolon: Self::Output,
        constant_name: Self::Output,
    ) -> Self::Output {
        let id = match self.expect_name(&constant_name) {
            Some(id) => id,
            None => return Node::Ignored(SK::TypeConstant),
        };
        let pos = self.merge_positions(&ty, &constant_name);
        let ty = match (&ty, self.get_current_classish_name()) {
            (Node::Name(self_name, self_pos), Some((class_name, class_name_pos)))
                if self_name == "self" =>
            {
                // In classes, we modify the position when rewriting the
                // `self` keyword to point to the class name. In traits,
                // we don't (because traits are not types). We indicate
                // that the position shouldn't be rewritten with the
                // none Pos.
                let id_pos = if class_name_pos.is_none() {
                    self_pos.clone()
                } else {
                    class_name_pos.clone()
                };
                let reason = Reason::FromWitnessDecl(WitnessDecl::Hint(self_pos.clone()));
                let ty_ = Ty_::Tapply((id_pos, class_name.into()), vec![]);
                Ty(reason, Box::new(ty_))
            }
            _ => match self.node_to_ty(ty) {
                Some(ty) => ty,
                None => return Node::Ignored(SK::TypeConstant),
            },
        };
        let reason = Reason::FromWitnessDecl(WitnessDecl::Hint(pos));
        // The reason-rewriting here is only necessary to match the
        // behavior of OCaml decl (which flattens and then unflattens
        // Haccess hints, losing some position information).
        let ty = Self::rewrite_taccess_reasons(ty, reason.clone());
        Node::Ty(Box::new(Ty(
            reason,
            Box::new(Ty_::Taccess(TaccessType(ty, id.into()))),
        )))
    }

    fn make_type_in_refinement(
        &mut self,
        _type_keyword: Self::Output,
        type_constant_name: Self::Output,
        _type_params: Self::Output,
        constraints: Self::Output,
        _equal_token: Self::Output,
        type_specifier: Self::Output,
    ) -> Self::Output {
        let Id(_, id) = match self.expect_name(&type_constant_name) {
            Some(id) => id,
            None => return Node::Ignored(SK::TypeInRefinement),
        };
        let bound = if type_specifier.is_ignored() {
            // A loose refinement, with bounds
            let (lower, upper) = self.partition_type_bounds_into_lower_and_upper(constraints);
            RefinedConstBound::TRloose(RefinedConstBounds { lower, upper })
        } else {
            // An exact refinement
            let ty = match self.node_to_ty(type_specifier) {
                Some(ty) => ty,
                None => return Node::Ignored(SK::TypeInRefinement),
            };
            RefinedConstBound::TRexact(ty)
        };
        Node::RefinedConst(
            id,
            Box::new(RefinedConst {
                bound,
                is_ctx: false,
            }),
        )
    }

    fn make_ctx_in_refinement(
        &mut self,
        _ctx_keyword: Self::Output,
        ctx_constant_name: Self::Output,
        _type_params: Self::Output,
        constraints: Self::Output,
        _equal_token: Self::Output,
        ctx_list: Self::Output,
    ) -> Self::Output {
        let Id(_, id) = match self.expect_name(&ctx_constant_name) {
            Some(id) => id,
            None => return Node::Ignored(SK::TypeInRefinement),
        };
        let bound = if ctx_list.is_ignored() {
            // A loose refinement, with bounds
            let (lower, upper) = self.partition_ctx_bounds_into_lower_and_upper(constraints);
            RefinedConstBound::TRloose(RefinedConstBounds { lower, upper })
        } else {
            // An exact refinement
            let ty = match self.node_to_ty(ctx_list) {
                Some(ty) => ty,
                None => return Node::Ignored(SK::TypeInRefinement),
            };
            RefinedConstBound::TRexact(ty)
        };
        Node::RefinedConst(
            id,
            Box::new(RefinedConst {
                bound,
                is_ctx: true,
            }),
        )
    }

    fn make_type_refinement(
        &mut self,
        root_type: Self::Output,
        _with_keyword: Self::Output,
        _left_brace: Self::Output,
        members: Self::Output,
        right_brace: Self::Output,
    ) -> Self::Output {
        let pos = self.merge_positions(&root_type, &right_brace);
        let reason = Reason::FromWitnessDecl(WitnessDecl::Hint(pos));
        let root_type = match self.node_to_ty(root_type) {
            Some(ty) => ty,
            None => return Node::Ignored(SK::TypeRefinement),
        };
        let const_members = members
            .into_iter()
            .filter_map(|node| match node {
                Node::ListItem(box node, _) | node => match node {
                    Node::RefinedConst(id, box ctr) => Some((id, ctr)),
                    _ => None,
                },
            })
            .collect();
        let class_ref = ClassRefinement {
            cr_consts: const_members,
        };
        Node::Ty(Box::new(Ty(
            reason,
            Box::new(Ty_::Trefinement(root_type, class_ref)),
        )))
    }

    // A type specifier preceded by an attribute list. At the time of writing,
    // only the <<__Soft>> attribute is permitted here.
    fn make_attributized_specifier(
        &mut self,
        attributes: Self::Output,
        hint: Self::Output,
    ) -> Self::Output {
        match attributes {
            Node::BracketedList(box (ltlt_pos, nodes, gtgt_pos)) => match &nodes[..] {
                [
                    Node::Attribute(box UserAttributeNode {
                        name: Id(_, attr_name),
                        ..
                    }),
                ] if attr_name.as_str() == "__Soft" => {
                    let attributes_pos = Self::merge(ltlt_pos, gtgt_pos);
                    let hint_pos = self.get_pos(&hint);
                    // Use the type of the hint as-is (i.e., throw away the
                    // knowledge that we had a soft type specifier here--the
                    // typechecker does not use it). Replace its Reason with one
                    // including the position of the attribute list.
                    let hint = match self.node_to_ty(hint) {
                        Some(ty) => ty,
                        None => return Node::Ignored(SK::AttributizedSpecifier),
                    };

                    self.hint_ty(
                        Self::merge(attributes_pos, hint_pos),
                        if self.opts.interpret_soft_types_as_like_types {
                            Ty_::Tlike(hint)
                        } else {
                            *hint.1
                        },
                    )
                }
                _ => hint,
            },
            _ => hint,
        }
    }

    fn make_vector_type_specifier(
        &mut self,
        vec: Self::Output,
        _left_angle: Self::Output,
        hint: Self::Output,
        _trailing_comma: Self::Output,
        right_angle: Self::Output,
    ) -> Self::Output {
        let id = match self.expect_name(&vec) {
            Some(id) => id,
            None => return Node::Ignored(SK::VectorTypeSpecifier),
        };
        let id = (id.0, self.elaborate_raw_id(&id.1).into_owned());
        self.make_apply(id, hint, self.get_pos(&right_angle))
    }

    fn make_dictionary_type_specifier(
        &mut self,
        dict: Self::Output,
        _left_angle: Self::Output,
        type_arguments: Self::Output,
        right_angle: Self::Output,
    ) -> Self::Output {
        let id = match self.expect_name(&dict) {
            Some(id) => id,
            None => return Node::Ignored(SK::DictionaryTypeSpecifier),
        };
        let id = (id.0, self.elaborate_raw_id(&id.1).into_owned());
        self.make_apply(id, type_arguments, self.get_pos(&right_angle))
    }

    fn make_keyset_type_specifier(
        &mut self,
        keyset: Self::Output,
        _left_angle: Self::Output,
        hint: Self::Output,
        _trailing_comma: Self::Output,
        right_angle: Self::Output,
    ) -> Self::Output {
        let id = match self.expect_name(&keyset) {
            Some(id) => id,
            None => return Node::Ignored(SK::KeysetTypeSpecifier),
        };
        let id = (id.0, self.elaborate_raw_id(&id.1).into_owned());
        self.make_apply(id, hint, self.get_pos(&right_angle))
    }

    fn make_variable_expression(&mut self, _expression: Self::Output) -> Self::Output {
        Node::Ignored(SK::VariableExpression)
    }

    fn make_file_attribute_specification(
        &mut self,
        _left_double_angle: Self::Output,
        _keyword: Self::Output,
        _colon: Self::Output,
        attributes: Self::Output,
        _right_double_angle: Self::Output,
    ) -> Self::Output {
        let keep_user_attributes = self.opts.keep_user_attributes;
        let this = Rc::make_mut(&mut self.state);
        let mut file_attributes = Vec::new();
        for attr in attributes.into_iter() {
            match attr {
                Node::Attribute(attr) => {
                    if attr.name.1 == naming_special_names::user_attributes::PACKAGE_OVERRIDE {
                        if let &[AttributeParam::String(pos, s)] = &attr.params.as_slice() {
                            let package_name =
                                std::str::from_utf8(s).expect("Unable to parse package override");
                            let package_override = nast::PackageMembership::PackageOverride(
                                pos.clone(),
                                package_name.into(),
                            );
                            this.package = Some(package_override);
                        }
                    }
                    if keep_user_attributes {
                        file_attributes.push(this.user_attribute_to_decl(*attr))
                    }
                }
                _ => {}
            }
        }
        file_attributes.reverse();
        this.file_attributes = file_attributes;
        Node::Ignored(SK::FileAttributeSpecification)
    }

    fn make_subscript_expression(
        &mut self,
        _receiver: Self::Output,
        _left_bracket: Self::Output,
        _index: Self::Output,
        _right_bracket: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::SubscriptExpression)
    }

    fn make_member_selection_expression(
        &mut self,
        _object: Self::Output,
        _operator: Self::Output,
        _name: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::MemberSelectionExpression)
    }

    fn make_object_creation_expression(
        &mut self,
        _new_keyword: Self::Output,
        _object: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::ObjectCreationExpression)
    }

    fn make_safe_member_selection_expression(
        &mut self,
        _object: Self::Output,
        _operator: Self::Output,
        _name: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::SafeMemberSelectionExpression)
    }

    fn make_function_call_expression(
        &mut self,
        _receiver: Self::Output,
        _type_args: Self::Output,
        _left_paren: Self::Output,
        _argument_list: Self::Output,
        _right_paren: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::FunctionCallExpression)
    }

    fn make_list_expression(
        &mut self,
        _keyword: Self::Output,
        _left_paren: Self::Output,
        _members: Self::Output,
        _right_paren: Self::Output,
    ) -> Self::Output {
        Node::Ignored(SK::ListExpression)
    }

    fn make_module_declaration(
        &mut self,
        _attributes: Self::Output,
        _new_keyword: Self::Output,
        _module_keyword: Self::Output,
        name: Self::Output,
        _left_brace: Self::Output,
        _right_brace: Self::Output,
    ) -> Self::Output {
        if let Node::ModuleName(parts, pos) = name {
            let module_name = self.module_name_string_from_parts(parts, pos.clone());
            let module = shallow_decl_defs::ModuleDefType { mdt_pos: pos };
            let this = Rc::make_mut(&mut self.state);
            this.module_decl(module_name, module)
        } else {
            Node::Ignored(SK::ModuleDeclaration)
        }
    }

    fn make_module_membership_declaration(
        &mut self,
        _module_keyword: Self::Output,
        name: Self::Output,
        _semicolon: Self::Output,
    ) -> Self::Output {
        match name {
            Node::ModuleName(parts, pos) => {
                if self.module.is_none() {
                    let name = self.module_name_string_from_parts(parts, pos.clone());
                    let this = Rc::make_mut(&mut self.state);
                    this.module = Some(oxidized::ast::Id(pos, name));
                }
            }
            _ => {}
        }
        Node::Ignored(SK::ModuleMembershipDeclaration)
    }

    /// See `DeclarationParser::parse_script` to see how the inner decls get
    /// collected into a Node::List. We explicitly return so it doesn't get
    /// filtered out as a zero node.
    fn make_script(&mut self, declarations: Self::Output) -> Self::Output {
        declarations
    }
}

#[cfg(test)]
mod tests {
    use std::borrow::Cow;

    use oxidized::aast;
    use oxidized::ast_defs::Id;
    use oxidized::pos::Pos;
    use oxidized::typing_defs::TaccessType;
    use oxidized::typing_defs::Ty;
    use oxidized::typing_defs::Ty_;
    use oxidized::typing_reason::Reason;
    use parser_core_types::token_kind::TokenKind;

    use super::DirectDeclSmartConstructors;
    use super::Modifiers;
    use super::NamespaceBuilder;
    use super::Node;
    use super::fixed_width_token::FixedWidthToken;
    use super::prefix_slash;
    use super::read_member_modifiers;
    use super::strip_dollar_prefix;

    // The polymorphic-context predicates below are associated functions on the
    // lifetime-parameterized DirectDeclSmartConstructors, but none of them touch
    // those lifetimes, so any concrete choice works when naming the type.
    type Ddsc = DirectDeclSmartConstructors<'static, 'static>;

    fn token(kind: TokenKind) -> Node {
        Node::Token(FixedWidthToken::new(kind, 0))
    }

    // prefix_slash --------------------------------------------------------

    // Invariant: prefix_slash always prepends a backslash and does not check for
    // one already present, so it is not idempotent on already-qualified names.
    #[test]
    fn prefix_slash_is_not_idempotent() {
        assert_eq!(prefix_slash("Foo"), "\\Foo");
        assert_eq!(prefix_slash(""), "\\");
        assert_eq!(prefix_slash("\\Foo"), "\\\\Foo");
    }

    // strip_dollar_prefix -------------------------------------------------

    // Invariant: strip_dollar_prefix removes every leading '$' (not just one) and
    // leaves interior '$' characters untouched.
    #[test]
    fn strip_dollar_prefix_strips_all_leading_dollars_only() {
        assert_eq!(strip_dollar_prefix("$x"), "x");
        assert_eq!(strip_dollar_prefix("x"), "x");
        assert_eq!(strip_dollar_prefix("$$x"), "x");
        assert_eq!(strip_dollar_prefix("x$y"), "x$y");
    }

    // Node::as_visibility -------------------------------------------------

    // Invariant: as_visibility maps exactly the four visibility keyword tokens and
    // returns None for any other token kind or for a non-token node.
    #[test]
    fn as_visibility_maps_only_visibility_keywords() {
        assert!(matches!(
            token(TokenKind::Public).as_visibility(),
            Some(aast::Visibility::Public)
        ));
        assert!(matches!(
            token(TokenKind::Private).as_visibility(),
            Some(aast::Visibility::Private)
        ));
        assert!(matches!(
            token(TokenKind::Protected).as_visibility(),
            Some(aast::Visibility::Protected)
        ));
        assert!(matches!(
            token(TokenKind::Internal).as_visibility(),
            Some(aast::Visibility::Internal)
        ));
        // A non-visibility keyword token yields no visibility.
        assert!(token(TokenKind::Static).as_visibility().is_none());
        // A non-token node yields no visibility.
        assert!(Node::Missing(0).as_visibility().is_none());
    }

    // Node::as_id ---------------------------------------------------------

    // Invariant: as_id produces an Id only for Name/XhpName nodes; unlike
    // as_variable it rejects Variable nodes (and any token).
    #[test]
    fn as_id_accepts_name_and_xhpname_but_not_variable() {
        let Id(_, name) = token_name("Foo").as_id().expect("Name yields an Id");
        assert_eq!(name, "Foo");

        let Id(_, xhp) = Node::XhpName(":x:y".to_string(), Pos::NONE)
            .as_id()
            .expect("XhpName yields an Id");
        assert_eq!(xhp, ":x:y");

        // A Variable is accepted by as_variable but explicitly not by as_id.
        assert!(
            Node::Variable("$x".to_string(), Pos::NONE)
                .as_id()
                .is_none()
        );
        assert!(token(TokenKind::Public).as_id().is_none());
    }

    fn token_name(s: &str) -> Node {
        Node::Name(s.to_string(), Pos::NONE)
    }

    // read_member_modifiers -----------------------------------------------

    fn modifiers(kinds: &[TokenKind]) -> Modifiers {
        let nodes: Vec<Node> = kinds.iter().map(|&k| token(k)).collect();
        read_member_modifiers(nodes.iter())
    }

    // Invariant: each modifier keyword sets its OWN flag and leaves the others
    // false, so a swapped token->flag assignment in the source is caught here;
    // absent a visibility keyword, visibility stays at the Public default.
    #[test]
    fn read_member_modifiers_each_keyword_sets_only_its_flag() {
        let m = modifiers(&[TokenKind::Static]);
        assert!(m.is_static);
        assert!(!m.is_abstract && !m.is_final && !m.is_readonly);
        assert!(matches!(m.visibility, aast::Visibility::Public));

        let m = modifiers(&[TokenKind::Abstract]);
        assert!(m.is_abstract);
        assert!(!m.is_static && !m.is_final && !m.is_readonly);

        let m = modifiers(&[TokenKind::Final]);
        assert!(m.is_final);
        assert!(!m.is_static && !m.is_abstract && !m.is_readonly);

        let m = modifiers(&[TokenKind::Readonly]);
        assert!(m.is_readonly);
        assert!(!m.is_static && !m.is_abstract && !m.is_final);
    }

    // Invariant: when several visibility keywords appear the last one wins, except
    // for the protected+internal special case (checked separately below).
    #[test]
    fn read_member_modifiers_last_visibility_wins() {
        assert!(matches!(
            modifiers(&[TokenKind::Public, TokenKind::Private]).visibility,
            aast::Visibility::Private
        ));
        // Protected followed by a non-internal visibility is plain last-wins.
        assert!(matches!(
            modifiers(&[TokenKind::Protected, TokenKind::Public]).visibility,
            aast::Visibility::Public
        ));
    }

    // Invariant: protected and internal combine into ProtectedInternal regardless
    // of which of the two keywords comes first.
    #[test]
    fn read_member_modifiers_protected_internal_combines_either_order() {
        assert!(matches!(
            modifiers(&[TokenKind::Protected, TokenKind::Internal]).visibility,
            aast::Visibility::ProtectedInternal
        ));
        assert!(matches!(
            modifiers(&[TokenKind::Internal, TokenKind::Protected]).visibility,
            aast::Visibility::ProtectedInternal
        ));
    }

    // NamespaceBuilder ----------------------------------------------------

    fn ns_builder() -> NamespaceBuilder {
        NamespaceBuilder::new(Vec::new(), false, false)
    }

    // Invariant: a fresh builder is in the global namespace (None); each pushed
    // name is qualified against the current namespace with a backslash separator.
    #[test]
    fn push_namespace_qualifies_against_current() {
        let mut b = ns_builder();
        assert_eq!(b.current_namespace(), None);
        b.push_namespace(Some(Cow::Borrowed("Foo")));
        assert_eq!(b.current_namespace(), Some("Foo"));
        b.push_namespace(Some(Cow::Borrowed("Bar")));
        assert_eq!(b.current_namespace(), Some("Foo\\Bar"));
    }

    // Invariant: pushing an unnamed namespace inherits the current namespace name
    // rather than clearing it.
    #[test]
    fn push_unnamed_namespace_inherits_current() {
        let mut b = ns_builder();
        b.push_namespace(Some(Cow::Borrowed("Foo")));
        b.push_namespace(None);
        assert_eq!(b.current_namespace(), Some("Foo"));
    }

    // Invariant: pop_namespace unwinds pushed namespaces but never pops the global
    // namespace off the bottom of the stack (popping at global is a no-op).
    #[test]
    fn pop_namespace_never_pops_global() {
        let mut b = ns_builder();
        b.push_namespace(Some(Cow::Borrowed("Foo")));
        b.push_namespace(Some(Cow::Borrowed("Bar")));
        b.pop_namespace();
        assert_eq!(b.current_namespace(), Some("Foo"));
        b.pop_namespace();
        assert_eq!(b.current_namespace(), None);
        // Already global: popping again must be a no-op that leaves the global
        // namespace on the stack intact. Verify by pushing afterward -- if the
        // guard had let the stack empty, this push would qualify against nothing
        // (or panic); instead it re-qualifies cleanly from the root.
        b.pop_namespace();
        b.push_namespace(Some(Cow::Borrowed("Zed")));
        assert_eq!(b.current_namespace(), Some("Zed"));
    }

    // Invariant (documented on the source fn): push(Y);pop();push(X) is equivalent
    // to push(Y);push(X);pop_previous() — pop_previous drops the *previous*
    // namespace while keeping the most recent one, re-anchored to global.
    #[test]
    fn pop_previous_namespace_matches_pop_then_push() {
        let mut a = ns_builder();
        a.push_namespace(Some(Cow::Borrowed("Y")));
        a.pop_namespace();
        a.push_namespace(Some(Cow::Borrowed("X")));

        let mut b = ns_builder();
        b.push_namespace(Some(Cow::Borrowed("Y")));
        b.push_namespace(Some(Cow::Borrowed("X")));
        b.pop_previous_namespace();

        assert_eq!(a.current_namespace(), Some("X"));
        assert_eq!(b.current_namespace(), a.current_namespace());
    }

    // Invariant: pop_previous_namespace is a no-op unless at least two namespaces
    // are pushed above the global one (stack depth greater than two).
    #[test]
    fn pop_previous_namespace_noop_without_two_pushed() {
        let mut b = ns_builder();
        b.push_namespace(Some(Cow::Borrowed("Y")));
        b.pop_previous_namespace();
        assert_eq!(b.current_namespace(), Some("Y"));
    }

    // Polymorphic-context predicates --------------------------------------

    fn generic(name: &str) -> Ty {
        Ty(Reason::NoReason, Box::new(Ty_::Tgeneric(name.to_string())))
    }

    fn apply(name: &str, args: Vec<Ty>) -> Ty {
        Ty(
            Reason::NoReason,
            Box::new(Ty_::Tapply((Pos::NONE, name.to_string()), args)),
        )
    }

    fn access(root: Ty, id: &str) -> Ty {
        Ty(
            Reason::NoReason,
            Box::new(Ty_::Taccess(TaccessType(root, (Pos::NONE, id.to_string())))),
        )
    }

    // Invariant: taccess_root_is_generic walks a Taccess tree and is true iff the
    // innermost root is a generic parameter, false when it is a class application.
    #[test]
    fn taccess_root_is_generic_follows_the_access_root() {
        assert!(Ddsc::taccess_root_is_generic(&generic("T")));
        assert!(!Ddsc::taccess_root_is_generic(&apply("C", vec![])));
        assert!(Ddsc::taccess_root_is_generic(&access(generic("T"), "TC")));
        assert!(!Ddsc::taccess_root_is_generic(&access(
            apply("C", vec![]),
            "TC"
        )));
        // Recurses through nested accesses down to the generic root.
        assert!(Ddsc::taccess_root_is_generic(&access(
            access(generic("T"), "A"),
            "B"
        )));
    }

    // Invariant: has_polymorphic_context flags a context when it is an
    // argument-less Tapply whose name contains '$', a Taccess whose root is such a
    // Tapply, or a Taccess rooted at a generic. The tys.is_empty() guard means a
    // '$' name carrying type arguments does NOT count.
    #[test]
    fn has_polymorphic_context_detects_dollar_roots_and_generic_accesses() {
        assert!(!Ddsc::has_polymorphic_context(&[]));
        // Bare context generic (name carries '$'), no type args -> polymorphic.
        assert!(Ddsc::has_polymorphic_context(&[apply("Tctx$0", vec![])]));
        // Same '$' name but with type arguments fails the empty-args guard.
        assert!(!Ddsc::has_polymorphic_context(&[apply(
            "Tctx$0",
            vec![generic("T")]
        )]));
        // A plain class application without '$' is not polymorphic.
        assert!(!Ddsc::has_polymorphic_context(&[apply(
            "Container",
            vec![]
        )]));
        // A type-constant access rooted at a generic is polymorphic.
        assert!(Ddsc::has_polymorphic_context(&[access(generic("T"), "TC")]));
        // A type-constant access whose root is a '$'-named argument-less Tapply
        // also qualifies (the first match arm's Taccess alternative).
        assert!(Ddsc::has_polymorphic_context(&[access(
            apply("Tctx$0", vec![]),
            "TC"
        )]));
    }

    // Invariant: ctx_generic_for_generic_taccess_inner renders a nested generic
    // Taccess as "<generic>::<const>[::<const>...]", recursing through the tree.
    #[test]
    fn ctx_generic_for_generic_taccess_inner_renders_nested_accesses() {
        assert_eq!(
            Ddsc::ctx_generic_for_generic_taccess_inner(&generic("T"), "C"),
            "T::C"
        );
        assert_eq!(
            Ddsc::ctx_generic_for_generic_taccess_inner(&access(generic("T"), "C1"), "C2"),
            "T::C1::C2"
        );
    }
}
