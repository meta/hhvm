// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! `positioned_parser` produces concrete syntax tree parametrized with
//! `PositionedToken` / `PositionedTrivia`. This is probably what you want
//! to use for most applications.

use parser::NoState;
use parser::lexable_token::LexableToken;
use parser::lexer::Lexer;
use parser::parser::Parser;
use parser::parser_env::ParserEnv;
use parser::positioned_syntax::PositionedSyntax;
use parser::positioned_token::PositionedToken;
use parser::positioned_trivia::PositionedTrivium;
use parser::smart_constructors_wrappers::WithKind;
use parser::source_text::SourceText;
use parser::syntax_error::SyntaxError;
use parser::token_factory::SimpleTokenFactoryImpl;
use parser::token_kind::TokenKind;
use positioned_smart_constructors::*;

pub type SmartConstructors = WithKind<
    PositionedSmartConstructors<PositionedSyntax, SimpleTokenFactoryImpl<PositionedToken>, NoState>,
>;

pub type ScState = NoState;

pub fn parse_script<'a>(
    source: &SourceText<'a>,
    env: ParserEnv,
) -> (PositionedSyntax, Vec<SyntaxError>, NoState) {
    let sc = WithKind::new(PositionedSmartConstructors::new(
        NoState,
        SimpleTokenFactoryImpl::new(),
    ));
    let mut parser = Parser::new(source, env, sc);
    let root = parser.parse_script();
    let errors = parser.errors();
    let sc_state = parser.into_sc_state();
    (root, errors, sc_state)
}

/// Scan a token using the same keyword and case rules as expressions.
pub fn scan_token_kind(source_text: &SourceText<'_>, offset: usize) -> TokenKind {
    Lexer::make_at(
        source_text,
        offset,
        SimpleTokenFactoryImpl::<PositionedToken>::new(),
    )
    .next_token()
    .kind()
}

fn trivia_lexer<'a>(
    source_text: &SourceText<'a>,
    offset: usize,
) -> Lexer<'a, SimpleTokenFactoryImpl<PositionedToken>> {
    Lexer::make_at(source_text, offset, SimpleTokenFactoryImpl::new())
}

pub fn scan_leading_xhp_trivia(
    source_text: &SourceText<'_>,
    offset: usize,
    width: usize,
) -> Vec<PositionedTrivium> {
    trivia_lexer(source_text, offset).scan_leading_xhp_trivia_with_width(width)
}

pub fn scan_trailing_xhp_trivia(
    source_text: &SourceText<'_>,
    offset: usize,
) -> Vec<PositionedTrivium> {
    trivia_lexer(source_text, offset).scan_trailing_xhp_trivia()
}

pub fn scan_leading_php_trivia(
    source_text: &SourceText<'_>,
    offset: usize,
    width: usize,
) -> Vec<PositionedTrivium> {
    trivia_lexer(source_text, offset).scan_leading_php_trivia_with_width(width)
}

pub fn scan_trailing_php_trivia(
    source_text: &SourceText<'_>,
    offset: usize,
) -> Vec<PositionedTrivium> {
    trivia_lexer(source_text, offset).scan_trailing_php_trivia()
}
