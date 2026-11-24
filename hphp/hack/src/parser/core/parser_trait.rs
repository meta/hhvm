// Copyright (c) 2019, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use parser_core_types::lexable_token::LexableToken;
use parser_core_types::lexable_trivia::LexableTrivia;
use parser_core_types::syntax_error::Error;
use parser_core_types::syntax_error::SyntaxError;
use parser_core_types::syntax_error::SyntaxQuickfix;
use parser_core_types::syntax_error::{self as Errors};
use parser_core_types::token_factory::TokenFactory;
use parser_core_types::token_kind::TokenKind;
use parser_core_types::trivia_factory::TriviaFactory;
use parser_core_types::trivia_kind::TriviaKind;

use crate::lexer;
use crate::lexer::Lexer;
use crate::parser_env::ParserEnv;
use crate::smart_constructors::NodeType;
use crate::smart_constructors::SmartConstructors;
use crate::smart_constructors::Token;
use crate::smart_constructors::Trivia;

#[derive(PartialEq)]
pub enum SeparatedListKind {
    NoTrailing,
    TrailingAllowed,
    ItemsOptional,
}

// This could be a set of token kinds, but it's part of parser envirnoment that is often cloned,
// so trying to keep it small.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ExpectedTokens {
    Classish = 0b0001,
    Semicolon = 0b0010,
    RightParen = 0b0100,
    Visibility = 0b1000,
}
const ET_COUNT: u32 = 4;
const ET_MASK: ETMask = (1 << ET_COUNT) - 1;
type ETMask = u16; // mask of bits in first ET_COUNT bits

impl ExpectedTokens {
    pub fn contains(mask: ETMask, token: TokenKind) -> bool {
        use ExpectedTokens::*;
        let bit: ETMask = match token {
            TokenKind::Class | TokenKind::Trait | TokenKind::Interface => Classish as ETMask,
            TokenKind::Semicolon => Semicolon as ETMask,
            TokenKind::RightParen => RightParen as ETMask,
            TokenKind::Public | TokenKind::Protected | TokenKind::Private | TokenKind::Internal => {
                Visibility as ETMask
            }
            _ => 0_u16,
        };
        (bit & mask) != 0
    }

    fn from(bit: ETMask) -> ExpectedTokens {
        // debug_assert!((bit & (!bit+1)) == bit, "unexpected multiple set bits in {:#b}");
        use ExpectedTokens::*;
        match bit {
            0b0001 => Classish,
            0b0010 => Semicolon,
            0b0100 => RightParen,
            _ => Visibility,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ExpectedTokenVec(Vec<ETMask>);
impl ExpectedTokenVec {
    fn push(&mut self, et: ExpectedTokens) {
        let last_mask = *self.0.last().unwrap_or(&0) & ET_MASK;
        let bit = et as ETMask;
        self.0.push(bit | last_mask | (bit << ET_COUNT));
    }
    fn pop(&mut self) -> Option<ExpectedTokens> {
        self.0.pop().map(|x| ExpectedTokens::from(x >> ET_COUNT))
    }
    fn last_mask(&self) -> ETMask {
        self.0.last().map_or(0, |x| x >> ET_COUNT)
    }
    fn any_mask(&self) -> ETMask {
        self.0.last().map_or(0, |x| x & ET_MASK)
    }
}

#[derive(Debug, Clone)]
pub struct Context<T> {
    pub expected: ExpectedTokenVec,
    pub skipped_tokens: Vec<T>,
}

impl<T> Context<T> {
    pub fn empty() -> Self {
        Self {
            expected: ExpectedTokenVec(vec![]),
            skipped_tokens: vec![],
        }
    }

    fn expect_in_new_scope(&mut self, expected: ExpectedTokens) {
        self.expected.push(expected);
    }

    fn pop_scope(&mut self, expected: ExpectedTokens) {
        let scope = self.expected.pop().unwrap();
        assert_eq!(expected, scope)
    }

    fn expects(&self, token_kind: TokenKind) -> bool {
        ExpectedTokens::contains(self.expected.any_mask(), token_kind)
    }

    fn expects_here(&self, token_kind: TokenKind) -> bool {
        ExpectedTokens::contains(self.expected.last_mask(), token_kind)
    }
}

pub trait ParserTrait<'a, S>: Clone
where
    S: SmartConstructors,
    <S as SmartConstructors>::Output: NodeType,
{
    fn make(
        _: Lexer<'a, S::Factory>,
        _: ParserEnv,
        _: Context<Token<S>>,
        _: Vec<SyntaxError>,
        _: S,
    ) -> Self;
    fn add_error(&mut self, _: SyntaxError);
    fn into_parts(
        self,
    ) -> (
        Lexer<'a, S::Factory>,
        Context<Token<S>>,
        Vec<SyntaxError>,
        S,
    );
    fn lexer(&self) -> &Lexer<'a, S::Factory>;
    fn lexer_mut(&mut self) -> &mut Lexer<'a, S::Factory>;
    fn continue_from<P: ParserTrait<'a, S>>(&mut self, _: P);

    fn env(&self) -> &ParserEnv;

    fn sc_mut(&mut self) -> &mut S;

    fn skipped_tokens(&self) -> &[Token<S>];
    fn drain_skipped_tokens(&mut self) -> std::vec::Drain<'_, Token<S>>;

    fn context_mut(&mut self) -> &mut Context<Token<S>>;
    fn context(&self) -> &Context<Token<S>>;

    fn pos(&self) -> usize {
        self.lexer().offset()
    }

    fn add_skipped_token(&mut self, token: Token<S>) {
        self.context_mut().skipped_tokens.push(token)
    }

    fn expects(&self, kind: TokenKind) -> bool {
        self.context().expects(kind)
    }

    fn expects_here(&self, kind: TokenKind) -> bool {
        self.context().expects_here(kind)
    }

    fn expect_in_new_scope(&mut self, expected: ExpectedTokens) {
        self.context_mut().expect_in_new_scope(expected)
    }

    fn pop_scope(&mut self, expected: ExpectedTokens) {
        self.context_mut().pop_scope(expected)
    }

    // This function reports an error starting at the current location of the
    // parser. Setting on_whole_token=false will report the error only on trivia,
    // which is useful in cases such as when "a semicolon is expected here" before
    // the current node. However, setting on_whole_token=true will report the error
    // only on the non-trivia text of the next token parsed, which is useful
    // in cases like "flagging an entire token as an extra".
    fn with_error_impl(
        &mut self,
        on_whole_token: bool,
        message: Error,
        quickfixes: Vec<SyntaxQuickfix>,
    ) {
        let (start_offset, end_offset) = self.error_offsets(on_whole_token);
        let error = SyntaxError::make(start_offset, end_offset, message, quickfixes);
        self.add_error(error)
    }

    fn with_error(&mut self, message: Error, quickfixes: Vec<SyntaxQuickfix>) {
        self.with_error_impl(false, message, quickfixes)
    }

    fn with_error_on_whole_token(&mut self, message: Error, quickfixes: Vec<SyntaxQuickfix>) {
        self.with_error_impl(true, message, quickfixes)
    }

    fn next_token_with_tokenizer<F>(&mut self, tokenizer: F) -> Token<S>
    where
        F: Fn(&mut Lexer<'a, S::Factory>) -> Token<S>,
    {
        let token = tokenizer(self.lexer_mut());
        if !self.skipped_tokens().is_empty() {
            let start = self.lexer().start();
            let mut leading = self
                .sc_mut()
                .token_factory_mut()
                .trivia_factory_mut()
                .make();
            for t in self.drain_skipped_tokens() {
                let (t_leading, t_width, t_trailing) = t.into_trivia_and_width();
                leading.extend(t_leading);
                leading.push(Trivia::<S>::make_extra_token_error(start, t_width));
                leading.extend(t_trailing);
            }
            leading.extend(token.clone_leading());
            self.sc_mut()
                .token_factory_mut()
                .with_leading(token, leading)
        } else {
            token
        }
    }

    fn next_token(&mut self) -> Token<S> {
        self.next_token_with_tokenizer(|x| x.next_token())
    }

    fn next_token_no_trailing(&mut self) -> Token<S> {
        self.lexer_mut().next_token_no_trailing()
    }

    fn next_docstring_header(&mut self) -> (Token<S>, &'a [u8]) {
        self.lexer_mut().next_docstring_header()
    }

    fn next_token_in_string(&mut self, literal_kind: &lexer::StringLiteralKind) -> Token<S> {
        self.lexer_mut().next_token_in_string(literal_kind)
    }

    fn next_xhp_class_name_or_other(&mut self) -> S::Output {
        let token = self.next_xhp_class_name_or_other_token();
        match token.kind() {
            TokenKind::Namespace | TokenKind::Name => {
                let has_trailing_newline =
                    token.has_trailing_trivia_kind(TriviaKind::EndOfLine);
                let name_token = self.sc_mut().make_token(token);
                self.scan_remaining_qualified_name(name_token, has_trailing_newline)
            }
            TokenKind::Backslash => {
                let pos = self.pos();
                let missing = self.sc_mut().make_missing(pos);
                let backslash = self.sc_mut().make_token(token);
                self.scan_qualified_name(missing, backslash)
            }
            _ => self.sc_mut().make_token(token),
        }
    }

    fn next_xhp_children_name_or_other(&mut self) -> Token<S> {
        if self.is_next_xhp_category_name() {
            self.next_xhp_category_name()
        } else if self.env().enable_xhp_class_modifier {
            self.next_xhp_modifier_class_name_or_other_token()
        } else {
            self.next_xhp_class_name_or_other_token()
        }
    }

    // Used in conjunction with the following function. If you call next_token
    // when the parser is at the <<<, it will scan the entire file looking for an
    // ending to the heredoc, which could quickly get bad if there are many such
    // declarations in a file.
    fn peek_next_partial_token_is_triple_left_angle(&self) -> bool {
        let mut lexer = self.lexer().clone();
        lexer.scan_leading_php_trivia();
        let tparam_open = lexer.peek_char(0);
        let attr1 = lexer.peek_char(1);
        let attr2 = lexer.peek_char(2);
        tparam_open == '<' && attr1 == '<' && attr2 == '<'
    }

    // Type parameter/argument lists begin with < and can have attributes immediately
    // afterwards, so this peeks a token kind at the beginning of such a list. *)
    fn peek_token_kind_with_possible_attributized_type_list(&self) -> TokenKind {
        if self.peek_next_partial_token_is_triple_left_angle() {
            TokenKind::LessThan
        } else {
            self.peek_token_kind()
        }
    }

    // In the case of attributes on generics, one could write
    // function f<<<__Attr>> reify T, ...> or Awaitable<<<__Soft>> int>
    // The triple left angle is currently lexed as a HeredocStringLiteral,
    // but we can get around this by manually advancing the lexer one token
    // and returning a LeftAngle. Then, the next token will be a LeftAngleLeftAngle
    fn assert_left_angle_in_type_list_with_possible_attribute(&mut self) -> S::Output {
        let parser1 = self.clone();
        let lexer = self.lexer_mut();
        lexer.scan_leading_php_trivia();
        let tparam_open = lexer.peek_char(0);
        let attr1 = lexer.peek_char(1);
        let attr2 = lexer.peek_char(2);
        if tparam_open == '<' && attr1 == '<' && attr2 == '<' {
            lexer.advance(1);
            let start = lexer.start();
            let token_factory = self.sc_mut().token_factory_mut();
            let leading = token_factory.trivia_factory_mut().make();
            let trailing = token_factory.trivia_factory_mut().make();
            let token = token_factory.make(TokenKind::LessThan, start, 1, leading, trailing);
            self.sc_mut().make_token(token)
        } else {
            self.continue_from(parser1);
            self.assert_token(TokenKind::LessThan)
        }
    }

    fn assert_xhp_body_token(&mut self, kind: TokenKind) -> S::Output {
        self.assert_token_with_tokenizer(kind, |x: &mut Lexer<'a, S::Factory>| {
            x.next_xhp_body_token()
        })
    }

    fn peek_token_with_lookahead(&self, lookahead: usize) -> Token<S> {
        let mut lexer = self.lexer().clone();
        let mut i = 0;
        loop {
            if i == lookahead {
                // call peek_next_token instead of next_token for the last one to leverage
                // lexer caching
                return lexer.peek_next_token();
            }
            let _ = lexer.next_token();
            i += 1
        }
    }

    fn peek_token(&self) -> Token<S> {
        self.lexer().peek_next_token()
    }

    fn peek_token_kind(&self) -> TokenKind {
        self.peek_token().kind()
    }

    fn peek_token_kind_with_lookahead(&self, lookahead: usize) -> TokenKind {
        self.peek_token_with_lookahead(lookahead).kind()
    }

    fn fetch_token(&mut self) -> S::Output {
        let token = self.lexer_mut().next_token();
        self.sc_mut().make_token(token)
    }

    fn assert_token_with_tokenizer<F>(&mut self, kind: TokenKind, tokenizer: F) -> S::Output
    where
        F: Fn(&mut Lexer<'a, S::Factory>) -> Token<S>,
    {
        let token = self.next_token_with_tokenizer(tokenizer);
        if token.kind() != kind {
            panic!(
                "Expected {:?}, but got {:?}. This indicates a bug in the parser, regardless of how broken the input code is.",
                kind,
                token.kind()
            )
        }
        self.sc_mut().make_token(token)
    }

    fn assert_token(&mut self, kind: TokenKind) -> S::Output {
        self.assert_token_with_tokenizer(kind, |x: &mut Lexer<'_, S::Factory>| x.next_token())
    }

    fn token_text(&self, token: &Token<S>) -> &'a str {
        match token.leading_start_offset() {
            None => "", // unavailable for minimal tokens
            Some(leading_start_offset) => unsafe {
                std::str::from_utf8_unchecked(
                    self.lexer()
                        .source()
                        .sub(leading_start_offset + token.leading_width(), token.width()),
                )
            },
        }
    }

    fn current_token_text(&self) -> &'a str {
        self.token_text(&self.peek_token())
    }
    // If the next token is a name or keyword, scan it as a name.
    fn next_token_as_name(&mut self) -> Token<S> {
        // TODO: This isn't right.  Pass flags to the lexer.
        self.lexer_mut().next_token_as_name()
    }

    fn optional_token(&mut self, kind: TokenKind) -> S::Output {
        if self.peek_token_kind() == kind {
            let token = self.next_token();
            self.sc_mut().make_token(token)
        } else {
            let pos = self.pos();
            self.sc_mut().make_missing(pos)
        }
    }

    fn scan_qualified_name_worker(
        &mut self,
        mut name_opt: Option<S::Output>,
        mut parts: Vec<S::Output>,
        mut has_backslash: bool,
        mut name_has_trailing_newline: bool,
    ) -> (Vec<S::Output>, Option<S::Output>, bool) {
        loop {
            let mut parser1 = self.clone();
            let token = if parser1.is_next_xhp_class_name() {
                parser1.next_xhp_class_name()
            } else {
                parser1.next_token_as_name()
            };
            match (name_opt.is_some(), token.kind()) {
                (true, TokenKind::Backslash) if !name_has_trailing_newline => {
                    // found backslash on the same line, create item and recurse
                    self.continue_from(parser1);
                    let token = self.sc_mut().make_token(token);
                    let part = self.sc_mut().make_list_item(name_opt.unwrap(), token);
                    parts.push(part);
                    has_backslash = true;
                    name_opt = None;
                    name_has_trailing_newline = false;
                }
                (false, TokenKind::Name) => {
                    // found a name, recurse to look for backslash
                    self.continue_from(parser1);
                    name_has_trailing_newline =
                        token.has_trailing_trivia_kind(TriviaKind::EndOfLine);
                    let token = self.sc_mut().make_token(token);
                    name_opt = Some(token);
                    has_backslash = false;
                }
                (true, _) if parts.is_empty() => {
                    // have not found anything - return [] to indicate failure
                    return (parts, name_opt, false);
                }
                (true, _) => {
                    // next token is not part of qualified name but we've consume some
                    // part of the input - create part for name with missing backslash
                    // and return accumulated result
                    let pos = self.pos();
                    let missing = self.sc_mut().make_missing(pos);
                    let part = self.sc_mut().make_list_item(name_opt.unwrap(), missing);
                    // TODO(T25649779)
                    parts.push(part);
                    return (parts, None, false);
                }
                _ => {
                    // next token is not part of qualified name - return accumulated result
                    return (parts, name_opt, has_backslash);
                }
            }
        }
    }

    fn scan_remaining_qualified_name_extended(
        &mut self,
        name_token: S::Output,
        name_has_trailing_newline: bool,
    ) -> (S::Output, bool) {
        let (parts, name_token_opt, is_backslash) =
            self.scan_qualified_name_worker(Some(name_token), vec![], false, name_has_trailing_newline);
        if parts.is_empty() {
            (name_token_opt.unwrap(), is_backslash)
        } else {
            let pos = self.pos();
            let list_node = self.sc_mut().make_list(parts, pos);
            let name = self.sc_mut().make_qualified_name(list_node);
            (name, is_backslash)
        }
    }

    fn scan_qualified_name_extended(
        &mut self,
        missing: S::Output,
        backslash: S::Output,
    ) -> (S::Output, bool) {
        let head = self.sc_mut().make_list_item(missing, backslash);
        let parts = vec![head];
        let (parts, _, is_backslash) = self.scan_qualified_name_worker(None, parts, false, false);
        let pos = self.pos();
        let list_node = self.sc_mut().make_list(parts, pos);
        let name = self.sc_mut().make_qualified_name(list_node);
        (name, is_backslash)
    }

    fn scan_qualified_name(&mut self, missing: S::Output, backslash: S::Output) -> S::Output {
        let (name, _) = self.scan_qualified_name_extended(missing, backslash);
        name
    }
    // If the next token is a name or an non-reserved keyword, scan it as
    // a name otherwise as a keyword.
    //
    // NB: A "reserved" keyword is in practice a keyword that cannot be used
    // as a class name or function name, for example, control flow keywords or
    // declaration keywords are reserved.
    fn next_token_non_reserved_as_name(&mut self) -> Token<S> {
        self.next_token_with_tokenizer(|l| l.next_token_non_reserved_as_name())
    }

    fn scan_header(&mut self) -> (Option<Token<S>>, Option<(Token<S>, Option<Token<S>>)>) {
        self.lexer_mut().scan_header()
    }

    fn error_offsets(&mut self, on_whole_token: bool /* = false */) -> (usize, usize) {
        if on_whole_token {
            let token = self.peek_token();
            let start_offset = self.lexer().offset() + token.leading_width();
            let end_offset = start_offset + token.width();
            (start_offset, end_offset)
        } else {
            let start_offset = self.lexer().start();
            let end_offset = self.lexer().offset();
            (start_offset, end_offset)
        }
    }

    fn scan_name_or_qualified_name(&mut self) -> S::Output {
        let mut parser1 = self.clone();
        let token = parser1.next_token_non_reserved_as_name();
        match token.kind() {
            TokenKind::Namespace | TokenKind::Name => {
                self.continue_from(parser1);
                let has_trailing_newline =
                    token.has_trailing_trivia_kind(TriviaKind::EndOfLine);
                let token = self.sc_mut().make_token(token);
                self.scan_remaining_qualified_name(token, has_trailing_newline)
            }
            TokenKind::Backslash => {
                self.continue_from(parser1);

                let pos = self.pos();
                let missing = self.sc_mut().make_missing(pos);
                let token = self.sc_mut().make_token(token);
                self.scan_qualified_name(missing, token)
            }
            _ => {
                let pos = self.pos();
                self.sc_mut().make_missing(pos)
            }
        }
    }

    fn parse_alternate_if_block<F>(&mut self, parse_item: F) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let mut parser1 = self.clone();
        let block = parser1.parse_list_while(parse_item, |x: &Self| match x.peek_token_kind() {
            TokenKind::Else | TokenKind::Endif => false,
            _ => true,
        });
        if block.is_missing() {
            let pos = self.pos();
            let empty1 = self.sc_mut().make_missing(pos);
            let pos = self.pos();
            let empty2 = self.sc_mut().make_missing(pos);
            let es = self.sc_mut().make_expression_statement(empty1, empty2);
            let pos = self.pos();
            self.sc_mut().make_list(vec![es], pos)
        } else {
            self.continue_from(parser1);
            block
        }
    }

    fn parse_separated_list<F>(
        &mut self,
        separator_kind: TokenKind,
        allow_trailing: SeparatedListKind,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> (S::Output, bool)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let (x, y, _) = self.parse_separated_list_predicate(
            |x| x == separator_kind,
            allow_trailing,
            |x| x == close_kind,
            error,
            parse_item,
        );
        (x, y)
    }

    fn require_qualified_name(&mut self) -> S::Output {
        let mut parser1 = self.clone();
        let name = if parser1.is_next_xhp_class_name() {
            parser1.next_xhp_class_name()
        } else {
            parser1.next_token_non_reserved_as_name()
        };

        match name.kind() {
            TokenKind::Namespace | TokenKind::Name | TokenKind::XHPClassName => {
                self.continue_from(parser1);
                let has_trailing_newline =
                    name.has_trailing_trivia_kind(TriviaKind::EndOfLine);
                let token = self.sc_mut().make_token(name);
                self.scan_remaining_qualified_name(token, has_trailing_newline)
            }
            TokenKind::Backslash => {
                self.continue_from(parser1);
                let pos = self.pos();
                let missing = self.sc_mut().make_missing(pos);
                let backslash = self.sc_mut().make_token(name);
                self.scan_qualified_name(missing, backslash)
            }
            _ => {
                self.with_error(Errors::error1004, Vec::new());
                let pos = self.pos();
                self.sc_mut().make_missing(pos)
            }
        }
    }

    fn require_qualified_module_name(&mut self) -> S::Output {
        let mut parts = vec![];

        let next_token_kind = self.peek_token_kind();
        let (name, root_is_default) =
            // The default module name is allowed with no dots. It's also defined already in modules.hhi,
            // so user code won't be able to define it.
            if next_token_kind == TokenKind::Default {
                (self.require_name_allow_all_keywords(), true)
            } else {
                (self.require_name_allow_non_reserved(), false)
            };

        let dot = self.optional_token(TokenKind::Dot);
        let dot_is_missing = dot.is_missing();

        if !(name.is_missing()) {
            parts.push(self.sc_mut().make_list_item(name, dot));
        }

        if dot_is_missing {
            let pos = self.pos();
            let list_node = self.sc_mut().make_list(parts, pos);
            return self.sc_mut().make_module_name(list_node);
        }

        if root_is_default {
            self.with_error(Errors::error1066, Vec::new());
        }

        loop {
            let next_token_kind = self.peek_token_kind();
            let name =
                // The default module name is allowed with no dots. It's also defined already in modules.hhi,
                // so user code won't be able to define it.
                if next_token_kind == TokenKind::Default {
                    self.with_error(Errors::error1066, Vec::new());
                    self.require_name_allow_all_keywords()
                } else {
                    self.require_name_allow_non_reserved()
                };

            if name.is_missing() {
                break;
            }

            let dot = self.optional_token(TokenKind::Dot);
            let dot_is_missing = dot.is_missing();

            parts.push(self.sc_mut().make_list_item(name, dot));

            if dot_is_missing {
                break;
            }
        }

        let pos = self.pos();
        let list_node = self.sc_mut().make_list(parts, pos);
        self.sc_mut().make_module_name(list_node)
    }

    fn require_name(&mut self) -> S::Output {
        self.require_token(TokenKind::Name, Errors::error1004)
    }

    fn require_xhp_class_name(&mut self) -> S::Output {
        let token = self.next_xhp_modifier_class_name();
        self.sc_mut().make_token(token)
    }

    fn require_xhp_class_name_or_name(&mut self) -> S::Output {
        if self.is_next_xhp_class_name() {
            let token = self.next_xhp_class_name();
            self.sc_mut().make_token(token)
        } else {
            self.require_token(TokenKind::Name, Errors::error1004)
        }
    }

    /// Require that the next node is either:
    /// - A normal class name (`\w+`)
    /// - An XHP class name (`(:(\w-)+)+`)
    fn require_maybe_xhp_class_name(&mut self) -> S::Output {
        if self.is_next_xhp_class_name() {
            let token = self.next_xhp_class_name();
            self.sc_mut().make_token(token)
        } else {
            self.require_name_allow_non_reserved()
        }
    }

    fn require_function(&mut self) -> S::Output {
        self.require_token(TokenKind::Function, Errors::error1003)
    }

    fn require_variable(&mut self) -> S::Output {
        self.require_token(TokenKind::Variable, Errors::error1008)
    }

    fn require_colon(&mut self) -> S::Output {
        self.require_token(TokenKind::Colon, Errors::error1020)
    }

    fn require_left_brace(&mut self) -> S::Output {
        self.require_token(TokenKind::LeftBrace, Errors::error1034)
    }

    fn require_slashgt(&mut self) -> S::Output {
        self.require_token(TokenKind::SlashGreaterThan, Errors::error1029)
    }

    fn require_right_brace(&mut self) -> S::Output {
        self.require_token(TokenKind::RightBrace, Errors::error1006)
    }

    fn require_left_paren(&mut self) -> S::Output {
        self.require_token(TokenKind::LeftParen, Errors::error1019)
    }

    fn require_left_angle(&mut self) -> S::Output {
        self.require_token(TokenKind::LessThan, Errors::error1021)
    }

    fn require_right_angle(&mut self) -> S::Output {
        self.require_token(TokenKind::GreaterThan, Errors::error1013)
    }

    fn require_comma(&mut self) -> S::Output {
        self.require_token(TokenKind::Comma, Errors::error1054)
    }

    fn require_right_bracket(&mut self) -> S::Output {
        self.require_token(TokenKind::RightBracket, Errors::error1032)
    }

    fn require_equal(&mut self) -> S::Output {
        self.require_token(TokenKind::Equal, Errors::error1036)
    }

    fn require_arrow(&mut self) -> S::Output {
        self.require_token(TokenKind::EqualGreaterThan, Errors::error1028)
    }

    fn require_lambda_arrow(&mut self) -> S::Output {
        self.require_token(TokenKind::EqualEqualGreaterThan, Errors::error1046)
    }

    fn require_as(&mut self) -> S::Output {
        self.require_token(TokenKind::As, Errors::error1023)
    }

    fn require_while(&mut self) -> S::Output {
        self.require_token(TokenKind::While, Errors::error1018)
    }

    fn require_coloncolon(&mut self) -> S::Output {
        self.require_token(TokenKind::ColonColon, Errors::error1047)
    }

    fn require_name_or_variable_or_error(&mut self, error: Error) -> S::Output {
        let mut parser1 = self.clone();
        let token = parser1.next_token_as_name();
        match token.kind() {
            TokenKind::Namespace | TokenKind::Name => {
                self.continue_from(parser1);
                let has_trailing_newline =
                    token.has_trailing_trivia_kind(TriviaKind::EndOfLine);
                let token = self.sc_mut().make_token(token);
                self.scan_remaining_qualified_name(token, has_trailing_newline)
            }
            TokenKind::Variable => {
                self.continue_from(parser1);
                self.sc_mut().make_token(token)
            }
            _ => {
                // ERROR RECOVERY: Create a missing token for the expected token,
                // and continue on from the current token. Don't skip it.
                self.with_error(error, Vec::new());
                let pos = self.pos();
                self.sc_mut().make_missing(pos)
            }
        }
    }

    fn require_name_or_variable(&mut self) -> S::Output {
        self.require_name_or_variable_or_error(Errors::error1050)
    }

    fn require_xhp_class_name_or_name_or_variable(&mut self) -> S::Output {
        if self.is_next_xhp_class_name() {
            let token = self.next_xhp_class_name();
            self.sc_mut().make_token(token)
        } else {
            self.require_name_or_variable()
        }
    }

    fn require_name_allow_non_reserved(&mut self) -> S::Output {
        let mut parser1 = self.clone();
        let token = parser1.next_token_non_reserved_as_name();
        if token.kind() == TokenKind::Name {
            self.continue_from(parser1);
            self.sc_mut().make_token(token)
        } else {
            // ERROR RECOVERY: Create a missing token for the expected token,
            // and continue on from the current token. Don't skip it.
            self.with_error(Errors::error1004, Vec::new());
            let pos = self.pos();
            self.sc_mut().make_missing(pos)
        }
    }

    fn next_xhp_category_name(&mut self) -> Token<S> {
        self.lexer_mut().next_xhp_category_name()
    }

    // We have a number of issues involving xhp class names, which begin with
    // a colon and may contain internal colons and dashes.  These are some
    // helper methods to deal with them.
    fn is_next_name(&mut self) -> bool {
        self.lexer().is_next_name()
    }

    fn next_xhp_name(&mut self) -> Token<S> {
        assert!(self.is_next_name());
        self.lexer_mut().next_xhp_name()
    }

    fn next_xhp_class_name(&mut self) -> Token<S> {
        assert!(self.is_next_xhp_class_name());
        self.lexer_mut().next_xhp_class_name()
    }

    fn next_xhp_modifier_class_name(&mut self) -> Token<S> {
        self.lexer_mut().next_xhp_modifier_class_name()
    }

    fn require_xhp_name(&mut self) -> S::Output {
        if self.is_next_name() {
            let token = self.next_xhp_name();
            self.sc_mut().make_token(token)
        } else {
            // ERROR RECOVERY: Create a missing token for the expected token,
            // and continue on from the current token. Don't skip it.
            // TODO: Different error?
            self.with_error(Errors::error1004, Vec::new());
            let pos = self.pos();
            self.sc_mut().make_missing(pos)
        }
    }

    fn is_next_xhp_category_name(&mut self) -> bool {
        self.lexer().is_next_xhp_category_name()
    }

    fn parse_comma_list_allow_trailing<F>(
        &mut self,
        close_predicate: TokenKind,
        error: Error,
        parse_item: F,
    ) -> (S::Output, bool)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list(
            TokenKind::Comma,
            SeparatedListKind::TrailingAllowed,
            close_predicate,
            error,
            parse_item,
        )
    }

    fn parse_comma_list_allow_trailing_opt<F>(
        &mut self,
        close_predicate: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt(
            TokenKind::Comma,
            SeparatedListKind::TrailingAllowed,
            close_predicate,
            error,
            parse_item,
        )
    }

    fn parse_separated_list_predicate<P, SP, F>(
        &mut self,
        separator_predicate: SP,
        list_kind: SeparatedListKind,
        close_predicate: P,
        error: Error,
        parse_item: F,
    ) -> (S::Output, bool, TokenKind)
    where
        P: Fn(TokenKind) -> bool,
        SP: Fn(TokenKind) -> bool,
        F: Fn(&mut Self) -> S::Output,
    {
        let mut items = vec![];
        // Set this when we first see a separator
        let mut separator_kind = TokenKind::Empty;

        loop {
            // At this point we are expecting an item followed by a separator,
            // a close, or, if trailing separators are allowed, both
            let kind = self.peek_token_kind();
            if close_predicate(kind) || kind == TokenKind::EndOfFile {
                // ERROR RECOVERY: We expected an item but we found a close or
                // the end of the file. Make the item and separator both
                // "missing" and give an error.
                //
                // If items are optional and we found a close, the last item was
                // omitted and there was no error.

                if kind == TokenKind::EndOfFile || list_kind != SeparatedListKind::ItemsOptional {
                    self.with_error(error, Vec::new())
                };
                let pos = self.pos();
                let missing1 = self.sc_mut().make_missing(pos);
                let pos = self.pos();
                let missing2 = self.sc_mut().make_missing(pos);
                let list_item = self.sc_mut().make_list_item(missing1, missing2);
                // TODO(T25649779)
                items.push(list_item);
                break;
            } else if separator_predicate(kind) {
                if separator_kind == TokenKind::Empty {
                    separator_kind = kind;
                } else if separator_kind != kind {
                    self.with_error(Errors::error1063, Vec::new());
                }

                // ERROR RECOVERY: We expected an item but we got a separator.
                // Assume the item was missing, eat the separator, and move on.
                //
                // If items are optional, there was no error, so eat the separator and
                // continue.
                //
                // TODO: This could be poor recovery. For example:
                //
                //     function bar (Foo< , int blah)
                //
                // Plainly the type arg is missing, but the comma is not associated with
                // the type argument list, it's associated with the formal
                // parameter list.

                let token = self.next_token();
                if list_kind != SeparatedListKind::ItemsOptional {
                    self.with_error(error.clone(), Vec::new())
                }
                let pos = self.pos();
                let item = self.sc_mut().make_missing(pos);
                let separator = self.sc_mut().make_token(token);
                let list_item = self.sc_mut().make_list_item(item, separator);
                // TODO(T25649779)
                items.push(list_item)
            } else {
                // We got neither a close nor a separator; hopefully we're going
                // to parse an item followed by a close or separator.
                let item = parse_item(self);
                let kind = self.peek_token_kind();

                if close_predicate(kind) {
                    let pos = self.pos();
                    let missing = self.sc_mut().make_missing(pos);
                    let list_item = self.sc_mut().make_list_item(item, missing);
                    // TODO(T25649779)
                    items.push(list_item);
                    break;
                } else if separator_predicate(kind) {
                    if separator_kind == TokenKind::Empty {
                        separator_kind = kind;
                    } else if separator_kind != kind {
                        self.with_error(Errors::error1063, Vec::new());
                    }
                    let token = self.next_token();

                    let separator = self.sc_mut().make_token(token);
                    let list_item = self.sc_mut().make_list_item(item, separator);
                    // TODO(T25649779)
                    items.push(list_item);
                    let allow_trailing = list_kind != SeparatedListKind::NoTrailing;
                    // We got an item followed by a separator; what if the thing
                    // that comes next is a close?
                    if allow_trailing && close_predicate(self.peek_token_kind()) {
                        break;
                    }
                } else {
                    // ERROR RECOVERY: We were expecting a close or separator, but
                    // got neither. Bail out. Caller will give an error.
                    let pos = self.pos();
                    let missing = self.sc_mut().make_missing(pos);
                    let list_item = self.sc_mut().make_list_item(item, missing);
                    // TODO(T25649779)
                    items.push(list_item);
                    break;
                }
            }
        }
        let no_arg_is_missing = items.iter().all(|x| !x.is_missing());
        let pos = self.pos();
        let item_list = self.sc_mut().make_list(items, pos);
        (item_list, no_arg_is_missing, separator_kind)
    }

    fn parse_list_until_none<F>(&mut self, parse_item: F) -> S::Output
    where
        F: Fn(&mut Self) -> Option<S::Output>,
    {
        let mut acc = vec![];
        loop {
            let maybe_item = parse_item(self);
            match maybe_item {
                None => break,
                Some(item) => {
                    let is_missing = item.is_missing();
                    acc.push(item);
                    if self.peek_token_kind() == TokenKind::EndOfFile ||
                    // exit if parser did not make any progress
                        is_missing
                    {
                        break;
                    }
                }
            }
        }
        let pos = self.pos();
        self.sc_mut().make_list(acc, pos)
    }

    fn parse_separated_list_opt_predicate<P, F>(
        &mut self,
        separator_kind: TokenKind,
        allow_trailing: SeparatedListKind,
        close_predicate: P,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        P: Fn(TokenKind) -> bool,
        F: Fn(&mut Self) -> S::Output,
    {
        let kind = self.peek_token_kind();
        if close_predicate(kind) {
            let pos = self.pos();
            self.sc_mut().make_missing(pos)
        } else {
            let (items, _, _) = self.parse_separated_list_predicate(
                |x| x == separator_kind,
                allow_trailing,
                close_predicate,
                error,
                parse_item,
            );
            items
        }
    }

    fn is_next_xhp_class_name(&self) -> bool {
        self.lexer().is_next_xhp_class_name()
    }

    fn next_xhp_modifier_class_name_or_other_token(&mut self) -> Token<S> {
        if self.is_next_name() {
            self.next_xhp_modifier_class_name()
        } else {
            self.next_token()
        }
    }

    fn next_xhp_class_name_or_other_token(&mut self) -> Token<S> {
        if self.is_next_xhp_class_name() {
            self.next_xhp_class_name()
        } else {
            self.next_token()
        }
    }

    fn parse_separated_list_opt<F>(
        &mut self,
        separator_kind: TokenKind,
        allow_trailing: SeparatedListKind,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt_predicate(
            separator_kind,
            allow_trailing,
            |x| x == close_kind,
            error,
            parse_item,
        )
    }

    fn parse_comma_list_opt_allow_trailing<F>(
        &mut self,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt(
            TokenKind::Comma,
            SeparatedListKind::TrailingAllowed,
            close_kind,
            error,
            parse_item,
        )
    }

    fn parse_comma_list_opt<F>(
        &mut self,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt(
            TokenKind::Comma,
            SeparatedListKind::NoTrailing,
            close_kind,
            error,
            parse_item,
        )
    }

    fn parse_comma_list_opt_items_opt<F>(
        &mut self,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt(
            TokenKind::Comma,
            SeparatedListKind::ItemsOptional,
            close_kind,
            error,
            parse_item,
        )
    }

    fn parse_comma_list_opt_allow_trailing_predicate<P, F>(
        &mut self,
        close_kind: P,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        P: Fn(TokenKind) -> bool,
        F: Fn(&mut Self) -> S::Output,
    {
        self.parse_separated_list_opt_predicate(
            TokenKind::Comma,
            SeparatedListKind::TrailingAllowed,
            close_kind,
            error,
            parse_item,
        )
    }

    fn parse_comma_list<F>(
        &mut self,
        close_kind: TokenKind,
        error: Error,
        parse_item: F,
    ) -> S::Output
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let (items, _) = self.parse_separated_list(
            TokenKind::Comma,
            SeparatedListKind::NoTrailing,
            close_kind,
            error,
            parse_item,
        );
        items
    }

    fn parse_delimited_list<P>(
        &mut self,
        left_kind: TokenKind,
        left_error: Error,
        right_kind: TokenKind,
        right_error: Error,
        parse_items: P,
    ) -> (S::Output, S::Output, S::Output)
    where
        P: FnOnce(&mut Self) -> S::Output,
    {
        let left = self.require_token(left_kind, left_error);
        let items = parse_items(self);
        let right = self.require_token(right_kind, right_error);
        (left, items, right)
    }

    fn parse_braced_list<P>(&mut self, parse_items: P) -> (S::Output, S::Output, S::Output)
    where
        P: FnOnce(&mut Self) -> S::Output,
    {
        self.parse_delimited_list(
            TokenKind::LeftBrace,
            Errors::error1034,
            TokenKind::RightBrace,
            Errors::error1006,
            parse_items,
        )
    }

    fn parse_parenthesized_list<F>(&mut self, parse_items: F) -> (S::Output, S::Output, S::Output)
    where
        F: FnOnce(&mut Self) -> S::Output,
    {
        self.parse_delimited_list(
            TokenKind::LeftParen,
            Errors::error1019,
            TokenKind::RightParen,
            Errors::error1011,
            parse_items,
        )
    }

    fn parse_parenthesized_comma_list<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items =
            |x: &mut Self| x.parse_comma_list(TokenKind::RightParen, Errors::error1011, parse_item);

        self.parse_parenthesized_list(parse_items)
    }

    fn parse_parenthesized_comma_list_opt_allow_trailing<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items = |x: &mut Self| {
            x.parse_comma_list_opt_allow_trailing(
                TokenKind::RightParen,
                Errors::error1011,
                parse_item,
            )
        };

        self.parse_parenthesized_list(parse_items)
    }

    fn parse_parenthesized_comma_list_opt_items_opt<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items = |x: &mut Self| {
            x.parse_comma_list_opt_items_opt(TokenKind::RightParen, Errors::error1011, parse_item)
        };

        self.parse_parenthesized_list(parse_items)
    }

    fn parse_braced_comma_list_opt_allow_trailing<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items = |parser: &mut Self| {
            parser.parse_comma_list_opt_allow_trailing(
                TokenKind::RightBrace,
                Errors::error1006,
                parse_item,
            )
        };
        self.parse_braced_list(parse_items)
    }

    fn parse_bracketted_list<F>(&mut self, parse_items: F) -> (S::Output, S::Output, S::Output)
    where
        F: FnOnce(&mut Self) -> S::Output,
    {
        self.parse_delimited_list(
            TokenKind::LeftBracket,
            Errors::error1026,
            TokenKind::RightBracket,
            Errors::error1031,
            parse_items,
        )
    }

    fn parse_bracketted_comma_list_opt_allow_trailing<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items = |x: &mut Self| {
            x.parse_comma_list_opt_allow_trailing(
                TokenKind::RightBracket,
                Errors::error1031,
                parse_item,
            )
        };
        self.parse_bracketted_list(parse_items)
    }

    fn parse_double_angled_list<F>(&mut self, parse_items: F) -> (S::Output, S::Output, S::Output)
    where
        F: FnOnce(&mut Self) -> S::Output,
    {
        self.parse_delimited_list(
            TokenKind::LessThanLessThan,
            Errors::error1029,
            TokenKind::GreaterThanGreaterThan,
            Errors::error1029,
            parse_items,
        )
    }

    fn parse_double_angled_comma_list_allow_trailing<F>(
        &mut self,
        parse_item: F,
    ) -> (S::Output, S::Output, S::Output)
    where
        F: Fn(&mut Self) -> S::Output,
    {
        let parse_items = |x: &mut Self| {
            let (items, _) = x.parse_comma_list_allow_trailing(
                TokenKind::GreaterThanGreaterThan,
                Errors::error1029,
                parse_item,
            );
            items
        };
        self.parse_double_angled_list(parse_items)
    }

    fn scan_remaining_qualified_name(
        &mut self,
        name_token: S::Output,
        name_has_trailing_newline: bool,
    ) -> S::Output {
        let (name, _) =
            self.scan_remaining_qualified_name_extended(name_token, name_has_trailing_newline);
        name
    }

    // Parse with parse_item while a condition is met.
    fn parse_list_while<F, P>(&mut self, mut parse_item: F, predicate: P) -> S::Output
    where
        F: FnMut(&mut Self) -> S::Output,
        P: Fn(&Self) -> bool,
    {
        let mut items = vec![];
        loop {
            if self.peek_token_kind() == TokenKind::EndOfFile || !predicate(self) {
                break;
            };

            let lexer_before = self.lexer().clone();
            let result = parse_item(self);
            if result.is_missing() {
                // ERROR RECOVERY: If the item is was parsed as 'missing', then it means
                // the parser bailed out of that scope. So, pass on whatever's been
                // accumulated so far, but with a 'Missing' SyntaxNode prepended.
                items.push(result);
                break;
            }
            if lexer_before.start() == self.lexer().start()
                && lexer_before.offset() == self.lexer().offset()
            {
                // INFINITE LOOP PREVENTION: If parse_item does not actually make
                // progress, just bail
                items.push(result);
                break;
            }
            // Or if nothing's wrong, continue.
            items.push(result)
        }
        let pos = self.pos();
        self.sc_mut().make_list(items, pos)
    }

    fn parse_terminated_list<F>(&mut self, parse_item: F, terminator: TokenKind) -> S::Output
    where
        F: FnMut(&mut Self) -> S::Output,
    {
        let predicate = |x: &Self| x.peek_token_kind() != terminator;
        self.parse_list_while(parse_item, predicate)
    }

    fn skip_and_log_unexpected_token(&mut self, generate_error: bool) {
        if generate_error {
            let extra_str = &self.current_token_text();
            self.with_error_on_whole_token(Errors::error1057(extra_str), Vec::new())
        };
        let token = self.next_token();
        self.add_skipped_token(token)
    }

    // Returns true if the strings underlying two tokens are of the same length
    // but with one character different.
    fn one_character_different<'b>(str1: &'b [u8], str2: &'b [u8]) -> bool {
        if str1.len() != str2.len() {
            false
        } else {
            // both strings have same length
            let str_len = str1.len();
            for i in 0..str_len {
                if str1[i] != str2[i] {
                    // Allow only one mistake
                    return str1[i + 1..] == str2[i + 1..];
                }
            }
            true
        }
    }

    // Compare the text of the token we have in hand to the text of the
    // anticipated kind. Note: this automatically returns false for any
    // TokenKinds of length 1.
    fn is_misspelled_kind(kind: TokenKind, token_str: &str) -> bool {
        let tokenkind_str = kind.to_string().as_bytes();
        let token_str = token_str.as_bytes();
        if tokenkind_str.len() <= 1 {
            false
        } else {
            Self::one_character_different(tokenkind_str, token_str)
        }
    }

    fn is_misspelled_from<'b>(kind_list: &[TokenKind], token_str: &'b str) -> bool {
        kind_list
            .iter()
            .any(|x| Self::is_misspelled_kind(*x, token_str))
    }

    // If token_str is a misspelling (by our narrow definition of misspelling)
    // of a TokenKind from kind_list, return the TokenKind that token_str is a
    // misspelling of. Otherwise, return None.
    fn suggested_kind_from(kind_list: &[TokenKind], token_str: &str) -> Option<TokenKind> {
        kind_list.iter().find_map(|x| {
            if Self::is_misspelled_kind(*x, token_str) {
                Some(*x)
            } else {
                None
            }
        })
    }

    fn skip_and_log_misspelled_token(&mut self, required_kind: TokenKind) {
        let received_str = &self.current_token_text();
        let required_str = required_kind.to_string();
        self.with_error_on_whole_token(Errors::error1058(received_str, required_str), Vec::new());
        self.skip_and_log_unexpected_token(/* generate_error:*/ false)
    }

    fn require_token_one_of(&mut self, kinds: &[TokenKind], error: Error) -> S::Output {
        let token_kind = self.peek_token_kind();
        if kinds.iter().any(|x| *x == token_kind) {
            let token = self.next_token();
            self.sc_mut().make_token(token)
        } else {
            // ERROR RECOVERY: Look at the next token after this. Is it the one we
            // require? If so, process the current token as extra and return the next
            // one. Otherwise, create a missing token for what we required,
            // and continue on from the current token (don't skip it).
            let next_kind = self.peek_token_kind_with_lookahead(1);
            if kinds.iter().any(|x| *x == next_kind) {
                self.skip_and_log_unexpected_token(true);
                let token = self.next_token();
                self.sc_mut().make_token(token)
            } else {
                // ERROR RECOVERY: We know we didn't encounter an extra token.
                // So, as a second line of defense, check if the current token
                // is a misspelling, by our existing narrow definition of misspelling.
                let is_misspelling =
                    |k: &&TokenKind| Self::is_misspelled_kind(**k, self.current_token_text());
                let kind = kinds.iter().find(is_misspelling);
                match kind {
                    Some(kind) => {
                        self.skip_and_log_misspelled_token(*kind);
                        let pos = self.pos();
                        self.sc_mut().make_missing(pos)
                    }
                    None => {
                        self.with_error(error, Vec::new());
                        let pos = self.pos();
                        self.sc_mut().make_missing(pos)
                    }
                }
            }
        }
    }

    fn require_token(&mut self, kind: TokenKind, error: Error) -> S::Output {
        // Must behave as `require_token_one_of parser [kind] error`
        if self.peek_token_kind() == kind {
            let token = self.next_token();
            self.sc_mut().make_token(token)
        } else {
            // ERROR RECOVERY: Look at the next token after this. Is it the one we
            // require? If so, process the current token as extra and return the next
            // one. Otherwise, create a missing token for what we required,
            // and continue on from the current token (don't skip it).
            let next_kind = self.peek_token_kind_with_lookahead(1);
            if next_kind == kind {
                self.skip_and_log_unexpected_token(true);
                let token = self.next_token();
                self.sc_mut().make_token(token)
            } else {
                // ERROR RECOVERY: We know we didn't encounter an extra token.
                // So, as a second line of defense, check if the current token
                // is a misspelling, by our existing narrow definition of misspelling.
                if Self::is_misspelled_kind(kind, self.current_token_text()) {
                    self.skip_and_log_misspelled_token(kind);
                    let pos = self.pos();
                    self.sc_mut().make_missing(pos)
                } else {
                    self.with_error(error, Vec::new());
                    let pos = self.pos();
                    self.sc_mut().make_missing(pos)
                }
            }
        }
    }

    fn require_and_return_token(&mut self, kind: TokenKind, error: Error) -> Option<Token<S>> {
        if self.peek_token_kind() == kind {
            Some(self.next_token())
        } else {
            // ERROR RECOVERY: Look at the next token after this. Is it the one we
            // require? If so, process the current token as extra and return the next
            // one. Otherwise, create a missing token for what we required,
            // and continue on from the current token (don't skip it).
            let next_kind = self.peek_token_kind_with_lookahead(1);
            if next_kind == kind {
                self.skip_and_log_unexpected_token(true);
                Some(self.next_token())
            } else {
                // ERROR RECOVERY: We know we didn't encounter an extra token.
                // So, as a second line of defense, check if the current token
                // is a misspelling, by our existing narrow definition of misspelling.
                if Self::is_misspelled_kind(kind, self.current_token_text()) {
                    self.skip_and_log_misspelled_token(kind);
                    None
                } else {
                    self.with_error(error, Vec::new());
                    None
                }
            }
        }
    }

    fn require_name_allow_all_keywords(&mut self) -> S::Output {
        let mut parser1 = self.clone();
        let token = parser1.next_token_as_name();

        if token.kind() == TokenKind::Name {
            self.continue_from(parser1);
            self.sc_mut().make_token(token)
        } else {
            // ERROR RECOVERY: Create a missing token for the expected token,
            // and continue on from the current token. Don't skip it.
            self.with_error(Errors::error1004, Vec::new());
            let pos = self.pos();
            self.sc_mut().make_missing(pos)
        }
    }

    fn require_right_paren(&mut self) -> S::Output {
        self.require_token(TokenKind::RightParen, Errors::error1011)
    }

    fn require_semicolon_token(&mut self, saw_type_name: bool) -> Option<Token<S>> {
        match self.peek_token_kind() {
            TokenKind::Variable if saw_type_name => self
                .require_and_return_token(TokenKind::Semicolon, Errors::local_variable_with_type),
            _ => self.require_and_return_token(TokenKind::Semicolon, Errors::error1010),
        }
    }

    fn require_semicolon(&mut self) -> S::Output {
        self.require_token(TokenKind::Semicolon, Errors::error1010)
    }
}
