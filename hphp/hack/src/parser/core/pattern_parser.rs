// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use parser_core_types::syntax_error::SyntaxError;
use parser_core_types::syntax_error::{self as Errors};
use parser_core_types::token_kind::TokenKind;

use crate::lexer::Lexer;
use crate::parser_env::ParserEnv;
use crate::parser_trait::Context;
use crate::parser_trait::ParserTrait;
use crate::smart_constructors::NodeType;
use crate::smart_constructors::SmartConstructors;
use crate::smart_constructors::Token;
use crate::type_parser::TypeParser;

#[derive(Clone)]
pub struct PatternParser<'a, S>
where
    S: SmartConstructors,
    S::Output: NodeType,
{
    lexer: Lexer<'a, S::Factory>,
    env: ParserEnv,
    context: Context<Token<S>>,
    errors: Vec<SyntaxError>,
    sc: S,
}

impl<'a, S> ParserTrait<'a, S> for PatternParser<'a, S>
where
    S: SmartConstructors,
    S::Output: NodeType,
{
    fn make(
        lexer: Lexer<'a, S::Factory>,
        env: ParserEnv,
        context: Context<Token<S>>,
        errors: Vec<SyntaxError>,
        sc: S,
    ) -> Self {
        Self {
            lexer,
            env,
            context,
            errors,
            sc,
        }
    }

    fn into_parts(
        self,
    ) -> (
        Lexer<'a, S::Factory>,
        Context<Token<S>>,
        Vec<SyntaxError>,
        S,
    ) {
        (self.lexer, self.context, self.errors, self.sc)
    }

    fn lexer(&self) -> &Lexer<'a, S::Factory> {
        &self.lexer
    }

    fn lexer_mut(&mut self) -> &mut Lexer<'a, S::Factory> {
        &mut self.lexer
    }

    fn continue_from<P: ParserTrait<'a, S>>(&mut self, other: P) {
        let (lexer, context, errors, sc) = other.into_parts();
        self.lexer = lexer;
        self.context = context;
        self.errors = errors;
        self.sc = sc;
    }

    fn add_error(&mut self, error: SyntaxError) {
        self.errors.push(error)
    }

    fn env(&self) -> &ParserEnv {
        &self.env
    }

    fn sc_mut(&mut self) -> &mut S {
        &mut self.sc
    }

    fn drain_skipped_tokens(&mut self) -> std::vec::Drain<'_, Token<S>> {
        self.context.skipped_tokens.drain(..)
    }

    fn skipped_tokens(&self) -> &[Token<S>] {
        &self.context.skipped_tokens
    }

    fn context_mut(&mut self) -> &mut Context<Token<S>> {
        &mut self.context
    }

    fn context(&self) -> &Context<Token<S>> {
        &self.context
    }
}

impl<'a, S> PatternParser<'a, S>
where
    S: SmartConstructors,
    S::Output: NodeType,
{
    fn with_type_parser<F, U>(&mut self, f: F) -> U
    where
        F: Fn(&mut TypeParser<'a, S>) -> U,
    {
        let mut type_parser: TypeParser<'_, S> = TypeParser::make(
            self.lexer.clone(),
            self.env.clone(),
            self.context.clone(),
            self.errors.clone(),
            self.sc.clone(),
        );
        let res = f(&mut type_parser);
        self.continue_from(type_parser);
        res
    }

    fn parse_type_specifier(&mut self) -> S::Output {
        self.with_type_parser(|x: &mut TypeParser<'a, S>| {
            let allow_var = false;
            let allow_attr = false;
            x.parse_type_specifier(allow_var, allow_attr)
        })
    }

    pub fn parse_pattern(&mut self) -> S::Output {
        match self.peek_token_kind() {
            TokenKind::Variable => self.parse_variable_or_refinement_pattern(),
            TokenKind::Name => self.parse_constructor_or_refinement_pattern(),
            _ => {
                // ERROR RECOVERY: when encountering an invalid token, make the
                // whole pattern missing and continue on, starting at the
                // unexpected token.
                self.with_error(Errors::expected_pattern, Vec::new());
                self.sc.make_missing(self.pos())
            }
        }
    }

    fn parse_variable_or_refinement_pattern(&mut self) -> S::Output {
        let variable = self.assert_token(TokenKind::Variable);

        match self.peek_token_kind() {
            TokenKind::Colon => self.parse_refinement_pattern(variable),
            _ => self.sc.make_variable_pattern(variable),
        }
    }

    fn parse_constructor_or_refinement_pattern(&mut self) -> S::Output {
        let name = self.assert_token(TokenKind::Name);
        let name = self.scan_remaining_qualified_name(name, false);

        match self.peek_token_kind() {
            // NB: This is only a valid refinement pattern if `name` is a `Name`
            // token beginning with an underscore character (i.e., a wildcard).
            // If it isn't, we emit an error in a later pass.
            TokenKind::Colon => self.parse_refinement_pattern(name),
            _ => self.parse_constructor_pattern(name),
        }
    }

    fn parse_constructor_pattern(&mut self, name: S::Output) -> S::Output {
        // SPEC:
        //
        // constructor-pattern:
        //   name  args-opt
        //   qualified-name  args-opt
        //
        // args:
        //     (  pattern-list-opt  )
        //
        // Wildcard patterns (e.g., `match $x { _ => ... } }`) are parsed as
        // constructor patterns here. We transform constructor patterns where
        // the name is a single `Name` token beginning with an underscore
        // character into wildcard patterns during lowering.

        let (left, items, right) = if self.peek_token_kind() == TokenKind::LeftParen {
            self.parse_parenthesized_comma_list_opt_items_opt(Self::parse_pattern)
        } else {
            (
                self.sc.make_missing(self.pos()),
                self.sc.make_missing(self.pos()),
                self.sc.make_missing(self.pos()),
            )
        };
        self.sc.make_constructor_pattern(name, left, items, right)
    }

    fn parse_refinement_pattern(&mut self, variable: S::Output) -> S::Output {
        // SPEC:
        //
        // refinement-pattern:
        //   variable-name  :  type-specifier
        //   name  :  type-specifier
        //
        // The `name  :  type-specifier` form is only legal when the name token
        // begins with an underscore (i.e., it's a wildcard). We emit the error
        // in a later pass.

        let colon = self.assert_token(TokenKind::Colon);
        let type_specifier = self.parse_type_specifier();
        self.sc
            .make_refinement_pattern(variable, colon, type_specifier)
    }
}
