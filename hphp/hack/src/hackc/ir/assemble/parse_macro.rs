// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
#![feature(deref_patterns)]

use std::fmt;

use proc_macro2::Delimiter;
use proc_macro2::Span;
use proc_macro2::TokenStream;
use proc_macro2::TokenTree;
use proc_macro2::token_stream::IntoIter;
use quote::ToTokens;
use quote::quote;

type TokenIter = std::iter::Peekable<IntoIter>;

/// Macro for helping parsing tokens.
///
/// General use:
///   parse!(tokenizer, pat...)
///
/// (Pattern syntax very loosely based on Lalrpop)
///
/// Basic Patterns:
///   "token"       - matches tokens (like "(", "+", ",") or identifiers.
///   "\n"          - matches a newline.
///   id            - matches any identifier.
///   string        - matches any quoted string.
///   tok           - matches any token.
///   @<var>        - Fills in 'var' with the location of the next token.
///
/// Complex Patterns:
///   pat0 pat1... - matches 'pat0' followed by 'pat1' (repeatable).
///   <e:pat>     - matches 'pat' and binds the result to the new variable 'e'.
///   <mut e:pat> - matches 'pat' and binds the result to the new mut variable 'e'.
///   f           - calls f(tokenizer) to parse a value.
///   f(a, b)     - calls f(tokenizer, a, b) to parse a value.
///   basic_pat?  - match the pattern zero or one times.  The only allowable
///                 patterns are the basic patterns listed above.
///   pat,+       - expects one or more comma separated list of identifiers.
///   pat,*       - expects zero or more comma separated list of
///                 identifiers. Note that due to macro limitations the next
///                 pattern MUST be ')', '}' or ']'.
///   { xyzzy }   - inject rust code xyzzy into the matcher at this point.
///   [ tok0: pat... ; tok1: pat... ; tok2: pat... ]
///               - based on the next token choose a pattern to execute.  NOTE:
///                 The matching token is NOT consumed!
///   [ tok0: pat... ; tok1: pat... ; else: pat... ]
///               - based on the next token choose a pattern to execute. If no
///                 pattern matches then 'else' clause is executed.
///
/// See bottom of this file for tests which show examples of what code various
/// patterns translate into.
#[cfg(not(test))]
#[proc_macro]
#[proc_macro_error2::proc_macro_error]
pub fn parse(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    use proc_macro_error2::abort;
    let input = TokenStream::from(input);
    let res = match parse_macro(input) {
        Ok(res) => res,
        Err(err) => abort!(err.0, err.1),
    };

    res.into()
}

const DEBUG: bool = false;

#[derive(Debug)]
#[allow(dead_code)] // field `0` is never read
struct ParseError(Span, String);

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.1)
    }
}

type Result<T> = std::result::Result<T, ParseError>;

fn parse_macro(input: TokenStream) -> Result<TokenStream> {
    if DEBUG {
        eprintln!("INPUT: {}", input);
    }

    let mut it = input.into_iter().peekable();
    let it = &mut it;

    // Gather the tokenizer variable
    let tokenizer = it.expect_ident()?;
    it.expect_punct(',')?;

    let tree = Ast::parse(it)?;
    if let Some(t) = it.next() {
        return Err(t.error("Unexpected input"));
    }

    let state = State { tokenizer };

    let mut var_decl = TokenStream::new();
    state.collect_var_decl(&tree, &mut var_decl);

    let body = state.emit_ast(tree, Context::None);

    let result = quote!(
        #var_decl
        #body
    );

    if DEBUG {
        eprintln!("RESULT: {:?}\n{}", result, result);
    }

    Ok(result)
}

#[derive(Debug)]
enum Predicate {
    Any,
    AnyIdentifier,
    AnyString,
    EndOfLine,
    Identifier { expect: TokenStream, desc: String },
}

impl Predicate {
    fn to_stream(&self, tokenizer: &TokenTree, context: Context) -> TokenStream {
        match (context, self) {
            (Context::None, Predicate::Any) => {
                quote!(#tokenizer.expect_any_token()?)
            }
            (Context::None, Predicate::AnyIdentifier) => {
                quote!(#tokenizer.expect_any_identifier()?)
            }
            (Context::None, Predicate::AnyString) => {
                quote!(#tokenizer.expect_any_string()?)
            }
            (Context::None, Predicate::EndOfLine) => {
                quote!(#tokenizer.expect_eol()?)
            }
            (Context::None, Predicate::Identifier { expect, .. }) => {
                quote!(#tokenizer.expect_identifier(#expect)?)
            }
            (Context::Optional, Predicate::Any) => {
                quote!(#tokenizer.peek()?)
            }
            (Context::Optional, Predicate::AnyIdentifier) => {
                quote!(#tokenizer.next_if_any_identifier()?)
            }
            (Context::Optional, Predicate::AnyString) => {
                quote!(#tokenizer.next_if_any_string()?)
            }
            (Context::Optional, Predicate::EndOfLine) => {
                quote!(#tokenizer.next_if_eol()?)
            }
            (Context::Optional, Predicate::Identifier { expect, .. }) => {
                quote!(#tokenizer.next_if_identifier(#expect)?)
            }
            (Context::Test, Predicate::Any) => {
                quote!(#tokenizer.peek()?.is_some())
            }
            (Context::Test, Predicate::AnyIdentifier) => {
                quote!(#tokenizer.peek_is_any_identifier()?)
            }
            (Context::Test, Predicate::AnyString) => {
                quote!(#tokenizer.peek_is_any_string()?)
            }
            (Context::Test, Predicate::EndOfLine) => {
                quote!(#tokenizer.peek_is_eol()?)
            }
            (Context::Test, Predicate::Identifier { expect, .. }) => {
                quote!(#tokenizer.peek_is_identifier(#expect)?)
            }
        }
    }

    fn what(&self) -> &str {
        match self {
            Predicate::Any => "any",
            Predicate::AnyIdentifier => "id",
            Predicate::AnyString => "string",
            Predicate::EndOfLine => "eol",
            Predicate::Identifier { desc, .. } => desc,
        }
    }
}

#[derive(Debug)]
struct SelectCase {
    predicate: Predicate,
    pattern: Ast,
}

#[derive(Debug)]
enum Ast {
    Code {
        code: TokenStream,
    },
    CommaSeparated(Box<Ast>),
    FnCall {
        name: TokenStream,
        params: TokenStream,
    },
    Loc {
        name: TokenTree,
    },
    OneOrMore(Box<Ast>),
    Predicate(Predicate),
    Select {
        cases: Vec<SelectCase>,
        else_case: Option<Box<Ast>>,
    },
    Sequence(Vec<Ast>),
    VarBind {
        // name_decl is used at the name declaration site (`let #name_decl`) -
        // it includes things that the declaration wants (like 'mut').
        name_decl: TokenStream,
        // name_use is used at the name assignment site (`let #name_use = ...`)
        // - it strips things that the assignment doesn't want (like `mut`).
        name_use: TokenStream,
        value: Box<Ast>,
    },
    ZeroOrMore(Box<Ast>),
    ZeroOrOne(Box<Ast>),
}

impl Ast {
    fn is_predicate(&self) -> bool {
        matches!(self, Ast::Predicate(_))
    }
}

impl Ast {
    fn parse(it: &mut TokenIter) -> Result<Ast> {
        let mut result = Vec::new();

        loop {
            let t = it.expect_peek()?;
            if t.is_punct('<') {
                result.push(Self::parse_var_assign(it)?);
            } else if t.is_punct('@') {
                result.push(Self::parse_loc(it)?);
            } else {
                result.push(Self::parse_subpattern(it)?);
            };

            // Parse up until the end of the stream or the first semicolon (the
            // raw semicolon is used to terminate a select case).
            if it.peek().map_or(true, |t| t.is_punct(';')) {
                break;
            }
        }

        if result.len() == 1 {
            Ok(result.swap_remove(0))
        } else {
            Ok(Ast::Sequence(result))
        }
    }

    fn parse_var_assign(it: &mut TokenIter) -> Result<Ast> {
        // <e:pat>
        it.expect_punct('<')?;

        let mut name_decl = TokenStream::new();
        let mut name_use = TokenStream::new();
        loop {
            let t = it.expect_tt()?;
            if t.is_punct(':') {
                break;
            }
            if !t.is_exact_ident("mut") {
                t.to_tokens(&mut name_use);
            }
            t.to_tokens(&mut name_decl);
        }

        let value = Box::new(Self::parse_subpattern(it)?);
        it.expect_punct('>')?;

        Ok(Ast::VarBind {
            name_decl,
            name_use,
            value,
        })
    }

    fn parse_loc(it: &mut TokenIter) -> Result<Ast> {
        // @loc
        it.expect_punct('@')?;
        let name = it.expect_tt()?;
        Ok(Ast::Loc { name })
    }

    fn parse_subpattern(it: &mut TokenIter) -> Result<Ast> {
        let t = it.expect_peek()?;
        let pat = match t {
            TokenTree::Literal(_) => {
                // "("
                Self::parse_literal(it)?
            }
            TokenTree::Ident(_) => {
                // f
                // id
                Self::parse_ident(it)?
            }
            TokenTree::Group(group) if group.delimiter() == Delimiter::Brace => {
                let code = group.stream();
                it.next();
                Ast::Code { code }
            }
            TokenTree::Group(group) if group.delimiter() == Delimiter::Bracket => {
                Self::parse_select(it)?
            }
            _ => {
                return Err(t.error(format!("Pattern expected, not '{}' ({:?})", t, t)));
            }
        };

        // check for trailing comma
        let pat = if it.peek().is_some_and(|t| t.is_punct(',')) {
            it.next();
            let count = it.expect_tt()?;
            if count.is_punct('+') {
                // 1+
                Ast::CommaSeparated(Box::new(Ast::OneOrMore(Box::new(pat))))
            } else if count.is_punct('*') {
                // 0+
                Ast::CommaSeparated(Box::new(Ast::ZeroOrMore(Box::new(pat))))
            } else {
                return Err(count.error("'*' or '+' expected"));
            }
        } else {
            pat
        };

        // check for trailing +,*,?
        let pat = if it.peek().is_some_and(|t| t.is_punct('+')) {
            it.next();
            Ast::OneOrMore(Box::new(pat))
        } else {
            pat
        };
        let pat = if it.peek().is_some_and(|t| t.is_punct('*')) {
            it.next();
            Ast::ZeroOrMore(Box::new(pat))
        } else {
            pat
        };
        let pat = if it.peek().is_some_and(|t| t.is_punct('?')) {
            let t = it.next().unwrap();
            if !pat.is_predicate() {
                return Err(t.error("Error: '?' only works with basic patterns"));
            }
            Ast::ZeroOrOne(Box::new(pat))
        } else {
            pat
        };

        Ok(pat)
    }

    fn parse_literal(it: &mut TokenIter) -> Result<Ast> {
        let literal = it.expect_literal()?;
        let expected = literal.literal_string()?;
        let what = format!("'{}'", expected);
        let ast = match expected.as_str() {
            "\\n" => Ast::Predicate(Predicate::EndOfLine),
            _ => Ast::Predicate(Predicate::Identifier {
                expect: literal.to_token_stream(),
                desc: what,
            }),
        };

        Ok(ast)
    }

    fn parse_ident(it: &mut TokenIter) -> Result<Ast> {
        let t = it.expect_ident()?;
        let ast = if t.to_string() == "string" {
            Ast::Predicate(Predicate::AnyString)
        } else if t.to_string() == "tok" {
            Ast::Predicate(Predicate::Any)
        } else if t.to_string() == "id" {
            Ast::Predicate(Predicate::AnyIdentifier)
        } else {
            // f
            // f(a, b, c)
            let mut name = vec![t];

            while it.peek().is_some_and(|t| t.is_punct('.')) {
                name.push(it.next().unwrap());
                name.push(it.expect_ident()?);
            }

            if it.peek().is_some_and(TokenTree::is_group) {
                let group = it.expect_group()?;
                let stream = group.group_stream()?;
                Ast::FnCall {
                    name: quote!(#(#name)*),
                    params: quote!(, #stream),
                }
            } else {
                Ast::FnCall {
                    name: quote!(#(#name)*),
                    params: TokenStream::new(),
                }
            }
        };
        Ok(ast)
    }

    fn parse_select(it: &mut TokenIter) -> Result<Ast> {
        // [ tok0: pat... ; tok1: pat... ; tok2: pat... ]
        let mut it = it.expect_tt()?.group_stream()?.into_iter().peekable();
        let it = &mut it;

        let mut cases = Vec::new();
        let mut else_case = None;
        loop {
            let predicate = {
                let head_span = it.expect_peek()?.span();
                let sub = Ast::parse_subpattern(it)?;
                match sub {
                    Ast::Predicate(predicate) => Some(predicate),
                    Ast::FnCall { name, .. } if name.to_string() == "else" => None,
                    _ => {
                        return Err(ParseError(head_span, "Simple predicate expected".into()));
                    }
                }
            };

            it.expect_punct(':')?;
            let pattern = Ast::parse(it)?;

            if let Some(predicate) = predicate {
                cases.push(SelectCase { predicate, pattern });
            } else {
                else_case = Some(Box::new(pattern));
            }

            if let Some(t) = it.next() {
                if !t.is_punct(';') {
                    return Err(t.error("';' expected"));
                }
                // Allow a trailing ';'
                if it.peek().is_none() {
                    break;
                }
            } else {
                break;
            }

            if else_case.is_some() {
                break;
            }
        }

        it.expect_eos()?;

        Ok(Ast::Select { cases, else_case })
    }
}

#[derive(Copy, Clone, Eq, PartialEq)]
enum Context {
    None,
    Optional,
    Test,
}

struct State {
    tokenizer: TokenTree,
}

impl State {
    // I think clippy is wrong here...
    #[allow(clippy::only_used_in_recursion)]
    fn collect_var_decl(&self, ast: &Ast, output: &mut TokenStream) {
        match ast {
            Ast::Code { .. } | Ast::FnCall { .. } | Ast::Predicate(_) => {}

            Ast::CommaSeparated(sub)
            | Ast::OneOrMore(sub)
            | Ast::ZeroOrMore(sub)
            | Ast::ZeroOrOne(sub) => {
                self.collect_var_decl(sub, output);
            }
            Ast::Select { .. } => {
                // Select declares new vars in each case.
            }
            Ast::Sequence(subs) => {
                for sub in subs {
                    self.collect_var_decl(sub, output);
                }
            }
            Ast::VarBind { name_decl, .. } => {
                output.extend(quote!(
                    #[allow(clippy::needless_late_init)]
                    let #name_decl ;
                ));
            }
            Ast::Loc { name } => {
                output.extend(quote!(
                    #[allow(clippy::needless_late_init)]
                    let #name ;
                ));
            }
        }
    }

    #[allow(clippy::todo)]
    fn emit_ast(&self, ast: Ast, context: Context) -> TokenStream {
        let tokenizer = &self.tokenizer;
        match ast {
            Ast::Code { code } => code,
            Ast::CommaSeparated(deref!(sub)) => match sub {
                Ast::ZeroOrMore(deref!(sub)) => {
                    let sub = self.emit_ast(sub, context);
                    quote! {
                        if !#tokenizer.peek_is_close_operator()? {
                            parse_comma_list(#tokenizer, false, |#tokenizer| {
                                let value = #sub;
                                Ok(value)
                            })?
                        } else {
                            Vec::new()
                        }
                    }
                }
                Ast::OneOrMore(deref!(sub)) => {
                    let sub = self.emit_ast(sub, context);
                    quote! {
                        parse_comma_list(#tokenizer, false, |#tokenizer| {
                            let value = #sub;
                            Ok(value)
                        })?
                    }
                }
                _ => unreachable!(),
            },
            Ast::FnCall { name, params } => {
                quote!(#name(#tokenizer #params)?)
            }
            Ast::Loc { name } => {
                quote!(#name = #tokenizer.peek_loc())
            }
            Ast::OneOrMore(_) => {
                todo!("OneOrMore");
            }
            Ast::Predicate(pred) => pred.to_stream(&self.tokenizer, context),
            Ast::Select { cases, else_case } => {
                // if tokenizer.peek_token(predicate).is_some() {
                //   ... pattern ...
                // else if tokenizer.peek_token(predicate).is_some() {
                //   ... pattern ...
                // else {
                //   complain about everything
                // }

                let mut result = TokenStream::new();
                let mut expected = Vec::new();
                for case in cases {
                    let predicate = case.predicate.to_stream(&self.tokenizer, Context::Test);
                    let what = case.predicate.what();

                    let mut var_decl = TokenStream::new();
                    self.collect_var_decl(&case.pattern, &mut var_decl);

                    let pat = self.emit_ast(case.pattern, Context::None);
                    result.extend(quote!(if #predicate { #var_decl #pat } else));
                    expected.push(what.to_owned());
                }

                // else
                if let Some(deref!(else_case)) = else_case {
                    let mut var_decl = TokenStream::new();
                    self.collect_var_decl(&else_case, &mut var_decl);

                    let else_case = self.emit_ast(else_case, Context::None);
                    result.extend(quote!({ #var_decl #else_case }));
                } else {
                    let expected = if expected.len() > 2 {
                        expected[..expected.len() - 1].join(", ")
                            + " or "
                            + expected.last().unwrap()
                    } else {
                        expected[0].to_owned()
                    };
                    result.extend(quote!({
                        let t = #tokenizer.expect_any_token()?;
                        return Err(t.bail(format!("{} expected, not '{}'", #expected, t)))
                    }));
                }

                result
            }
            Ast::Sequence(subs) => {
                let mut result = TokenStream::new();
                let mut first = true;
                for sub in subs {
                    if !first {
                        result.extend(quote!(;));
                    }
                    first = false;
                    result.extend(self.emit_ast(sub, context));
                }
                result
            }
            Ast::VarBind {
                name_use,
                value: deref!(value),
                ..
            } => {
                let value = self.emit_ast(value, context);
                quote!(#name_use = #value)
            }
            Ast::ZeroOrMore(deref!(sub)) => {
                match sub {
                    Ast::Predicate(Predicate::AnyIdentifier) => {
                        // id
                        let sub = self.emit_ast(sub, Context::Optional);
                        quote!(
                            #tokenizer.parse_list(|#tokenizer| { Ok(#sub) })?
                        )
                    }
                    Ast::Predicate(Predicate::AnyString) => {
                        // string
                        let sub = self.emit_ast(sub, Context::Optional);
                        quote!(
                            #tokenizer.parse_list(|#tokenizer| { Ok(#sub) })?
                        )
                    }
                    _ => {
                        let sub = self.emit_ast(sub, Context::None);
                        quote!(
                            #tokenizer.parse_list(|#tokenizer| {
                                if #tokenizer.peek_is_identifier(";")? {
                                    Ok(None)
                                } else {
                                    Ok(Some(#sub))
                                }
                            })?
                        )
                    }
                }
            }
            Ast::ZeroOrOne(deref!(sub)) => self.emit_ast(sub, Context::Optional),
        }
    }
}

// Used to extend TokenTree with some useful methods.
trait MyTokenTree {
    fn error<T: Into<String>>(&self, msg: T) -> ParseError;
    fn group_stream(&self) -> Result<TokenStream>;
    fn is_exact_ident(&self, ident: &str) -> bool;
    fn is_group(&self) -> bool;
    fn is_literal(&self) -> bool;
    fn is_punct(&self, expected: char) -> bool;
    fn literal_string(&self) -> Result<String>;
    #[allow(dead_code)]
    fn with_span(&self, span: Span) -> TokenTree;
}

impl MyTokenTree for TokenTree {
    fn error<T: Into<String>>(&self, msg: T) -> ParseError {
        ParseError(self.span(), msg.into())
    }

    fn group_stream(&self) -> Result<TokenStream> {
        match self {
            TokenTree::Group(group) => Ok(group.stream()),
            _ => Err(ParseError(self.span(), "Group expected".into())),
        }
    }

    fn is_exact_ident(&self, ident: &str) -> bool {
        matches!(self, TokenTree::Ident(_)) && self.to_string() == ident
    }

    fn is_group(&self) -> bool {
        matches!(self, TokenTree::Group(_))
    }

    fn is_literal(&self) -> bool {
        matches!(self, TokenTree::Literal(_))
    }

    fn is_punct(&self, expected: char) -> bool {
        match self {
            TokenTree::Punct(punct) => punct.as_char() == expected,
            _ => false,
        }
    }

    fn literal_string(&self) -> Result<String> {
        let s = self.to_string();
        let s = s.strip_prefix('"').and_then(|s| s.strip_suffix('"'));
        s.ok_or_else(|| self.error("Quoted literal expected"))
            .map(|s| s.to_owned())
    }

    fn with_span(&self, span: Span) -> TokenTree {
        let mut tt = self.clone();
        tt.set_span(span);
        tt
    }
}

// Used to extend TokenIter with some useful methods.
#[allow(dead_code)]
trait MyTokenIter {
    fn expect_peek(&mut self) -> Result<&TokenTree>;
    fn expect_tt(&mut self) -> Result<TokenTree>;
    fn expect_ident(&mut self) -> Result<TokenTree>;
    fn expect_token<F: FnOnce(&TokenTree) -> bool>(
        &mut self,
        what: &str,
        f: F,
    ) -> Result<TokenTree>;
    fn expect_literal(&mut self) -> Result<TokenTree>;
    fn expect_group(&mut self) -> Result<TokenTree>;
    fn expect_punct(&mut self, expected: char) -> Result<TokenTree>;
    fn expect_eos(&mut self) -> Result<()>;
}

impl MyTokenIter for TokenIter {
    fn expect_peek(&mut self) -> Result<&TokenTree> {
        if let Some(t) = self.peek() {
            Ok(t)
        } else {
            proc_macro_error2::Diagnostic::new(
                proc_macro_error2::Level::Error,
                "Token expected at end".into(),
            )
            .abort();
        }
    }

    fn expect_tt(&mut self) -> Result<TokenTree> {
        self.expect_peek()?;
        Ok(self.next().unwrap())
    }

    fn expect_ident(&mut self) -> Result<TokenTree> {
        let t = self.expect_tt()?;
        if !matches!(t, TokenTree::Ident(_)) {
            return Err(t.error("Identifier expected"));
        }
        Ok(t)
    }

    fn expect_token<F: FnOnce(&TokenTree) -> bool>(
        &mut self,
        what: &str,
        f: F,
    ) -> Result<TokenTree> {
        let t = self.expect_tt()?;
        if f(&t) {
            Ok(t)
        } else {
            Err(t.error(format!("{} expected", what)))
        }
    }

    fn expect_literal(&mut self) -> Result<TokenTree> {
        let t = self.expect_tt()?;
        if t.is_literal() {
            Ok(t)
        } else {
            Err(t.error("literal expected"))
        }
    }

    fn expect_group(&mut self) -> Result<TokenTree> {
        let t = self.expect_tt()?;
        if t.is_group() {
            Ok(t)
        } else {
            Err(t.error("Group expected"))
        }
    }

    fn expect_punct(&mut self, expected: char) -> Result<TokenTree> {
        let t = self.expect_tt()?;
        if t.is_punct(expected) {
            Ok(t)
        } else {
            Err(t.error(format!("'{}' expected", expected)))
        }
    }

    fn expect_eos(&mut self) -> Result<()> {
        if let Some(t) = self.next() {
            Err(t.error("unexpected token"))
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
#[rustfmt::skip] // skip rustfmt because it tends to add unwanted tokens to our quotes.
#[allow(dead_code)]
mod test {
    use super::parse_macro;
    use quote::quote;
    use macro_test_util::assert_pat_eq;
    use macro_test_util::assert_error;

    #[test]
    fn test_basic() {
        assert_pat_eq(
            parse_macro(quote!(t, "(")),
            quote!(t.expect_identifier("(")?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, "\n")),
            quote!(t.expect_eol()?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, "xyzzy")),
            quote!(t.expect_identifier("xyzzy")?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, id)),
            quote!(t.expect_any_identifier()?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, tok)),
            quote!(t.expect_any_token()?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, string)),
            quote!(t.expect_any_string()?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, "a" "b")),
            quote!(t.expect_identifier("a")?; t.expect_identifier("b")?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:string>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.expect_any_string()?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, <mut e:string>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let mut e;
                e = t.expect_any_string()?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, custom)),
            quote!(custom(t)?)
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:custom>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = custom(t)?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, custom(param))),
            quote!(custom(t, param)?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, self.custom)),
            quote!(self.custom(t)?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, self.custom(param))),
            quote!(self.custom(t, param)?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, id)),
            quote!(t.expect_any_identifier()?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, ".xyzzy")),
            quote!(t.expect_identifier(".xyzzy")?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, @loc "xyzzy")),
            quote!(
                #[allow(clippy::needless_late_init)]
                let loc;
                loc = t.peek_loc();
                t.expect_identifier("xyzzy")?
            ),
        );
    }

    #[test]
    fn test_optional() {
        assert_pat_eq(
            parse_macro(quote!(t, <e:"("?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_identifier("(")?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:"foo"?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_identifier("foo")?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:".foo"?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_identifier(".foo")?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:tok?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.peek()?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:id?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_any_identifier()?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:id?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_any_identifier()?
            )
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:string?>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.next_if_any_string()?
            )
        );

        assert_error(
            parse_macro(quote!(t, { foo(); }?)),
            "Error: '?' only works with basic patterns"
        );
    }

    #[test]
    fn test_list() {
        assert_pat_eq(
            parse_macro(quote!(t, <e:id*>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.parse_list(|t| { Ok(t.next_if_any_identifier()?) })?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:f*>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = t.parse_list(|t| {
                    if t.peek_is_identifier(";")? {
                        Ok(None)
                    } else {
                        Ok(Some(f(t)?))
                    }
                })?
            ),
        );
    }

    #[test]
    fn test_comma() {
        assert_pat_eq(
            parse_macro(quote!(t, "x",+)),
            quote!(parse_comma_list(t, false, |t| {
                let value = t.expect_identifier("x")?;
                Ok(value)
            })?),
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:"x",+>)),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = parse_comma_list(t, false, |t| {
                    let value = t.expect_identifier("x")?;
                    Ok(value)
                })?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, <e:"x",*> "]")),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                e = if !t.peek_is_close_operator()? {
                    parse_comma_list(t, false, |t| {
                        let value = t.expect_identifier("x")?;
                        Ok(value)
                    })?
                } else {
                    Vec::new()
                };
                t.expect_identifier("]")?
            ),
        );
    }

    #[test]
    fn test_embedded_code() {
        assert_pat_eq(
            parse_macro(quote!(t, "(" { something(); } ")")),
            quote!(
                t.expect_identifier("(")?;
                something();
                /* harmless extra semicolon -> */ ;
                t.expect_identifier(")")?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, "(" <e:{ something(); }> ")")),
            quote!(
                #[allow(clippy::needless_late_init)]
                let e;
                t.expect_identifier("(")?;
                e = something();
                /* harmless extra semicolon -> */ ;
                t.expect_identifier(")")?
            ),
        );
    }

    #[test]
    fn test_select() {
        assert_pat_eq(
            parse_macro(quote!(t, "(" [
                "a": parse_a;
                "b": <e:parse_b>;
                "(": parse_c;
            ] ")")),
            quote!(
                t.expect_identifier("(")?;
                if t.peek_is_identifier("a")? {
                    parse_a(t)?
                } else if t.peek_is_identifier("b")? {
                    #[allow(clippy::needless_late_init)]
                    let e;
                    e = parse_b(t)?
                } else if t.peek_is_identifier("(")? {
                    parse_c(t)?
                } else {
                    let t = t.expect_any_token()?;
                    return Err(t.bail(format!("{} expected, not '{}'" , "'a', 'b' or '('" , t)))
                }
                /* harmless extra semi -> */ ;
                t.expect_identifier(")")?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, [
                ".a": parse_a;
                else: parse_b;
            ])),
            quote!(
                if t.peek_is_identifier(".a")? {
                    parse_a(t)?
                } else {
                    parse_b(t)?
                }
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, [
                id: parse_a;
                else: parse_b;
            ])),
            quote!(
                if t.peek_is_any_identifier()? {
                    parse_a(t)?
                } else {
                    parse_b(t)?
                }
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, [
                tok: parse_a;
                else: parse_b;
            ])),
            quote!(
                if t.peek()?.is_some() {
                    parse_a(t)?
                } else {
                    parse_b(t)?
                }
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(t, "(" [
                "a": parse_a;
                "b": parse_b;
                else: parse_c;
            ] ")")),
            quote!(
                t.expect_identifier("(")?;
                if t.peek_is_identifier("a")? {
                    parse_a(t)?
                } else if t.peek_is_identifier("b")? {
                    parse_b(t)?
                } else {
                    parse_c(t)?
                }
                /* harmless extra semi -> */ ;
                t.expect_identifier(")")?
            ),
        );

        assert_pat_eq(
            parse_macro(quote!(tokenizer, [
                "as": "as" { Ok(DowncastReason::As) } ;
                "native": "native" <index:num> { Ok(DowncastReason::Native { index:index.as_int()? }) } ;
                "param": "param" <index:num> { Ok(DowncastReason::Param {index:index.as_int()? }) } ;
                "return": "return" { Ok(DowncastReason::Return) } ;
                else: <token:tok> { Err(token.error("Unknown VerifyIs reason")) } ;
            ])),
            quote!(
                if tokenizer.peek_is_identifier("as")? {
                    tokenizer.expect_identifier("as")?;
                    Ok(DowncastReason::As)
                } else if tokenizer.peek_is_identifier("native")? {
                    #[allow(clippy::needless_late_init)]
                    let index;
                    tokenizer.expect_identifier("native")?;
                    index = num(tokenizer)?;
                    Ok(DowncastReason::Native { index: index.as_int()? })
                } else if tokenizer.peek_is_identifier("param")? {
                    #[allow(clippy::needless_late_init)]
                    let index;
                    tokenizer.expect_identifier("param")?;
                    index = num(tokenizer)?;
                    Ok(DowncastReason::Param { index: index.as_int()? })
                } else if tokenizer.peek_is_identifier("return")? {
                    tokenizer.expect_identifier("return")?;
                    Ok(DowncastReason::Return)
                } else {
                    #[allow(clippy::needless_late_init)]
                    let token;
                    token = tokenizer.expect_any_token()?;
                    Err(token.error("Unknown VerifyIs reason"))
                }
            ),
        );
    }
}
