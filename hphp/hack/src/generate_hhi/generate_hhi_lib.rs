// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::ops::ControlFlow;
use std::ops::Range;
use std::path::Path;
use std::sync::Arc;

use parser_core_types::indexed_source_text::IndexedSourceText;
use parser_core_types::positioned_token::PositionedToken as Token;
use parser_core_types::source_text::SourceText;
use parser_core_types::syntax::Syntax;
use parser_core_types::syntax::SyntaxTypeBase;
use parser_core_types::syntax::SyntaxVariant;
use parser_core_types::token_factory::SimpleTokenFactory;
use parser_core_types::token_kind::TokenKind;
use parser_core_types::trivia_kind::TriviaKind;
use relative_path::Prefix;
use relative_path::RelativePath;

/// Writes an HHI for the Hack source file `filename` to `out`. If `source_path`
/// is given, each function's docblock records where the function is implemented
/// as `@source <source_path>:<line>`.
pub fn run(
    out: &mut impl std::io::Write,
    filename: &Path,
    source_path: Option<&str>,
) -> anyhow::Result<()> {
    let text = std::fs::read(filename)?;
    let text = match source_path {
        Some(source_path) => add_source_tags(filename, &text, source_path),
        None => text,
    };
    write_hhi(out, filename, &text)?;
    Ok(())
}

fn make_source_text<'a>(filename: &Path, text: &'a [u8]) -> SourceText<'a> {
    SourceText::make(
        Arc::new(RelativePath::make(Prefix::Dummy, filename.to_path_buf())),
        text,
    )
}

fn write_hhi(out: &mut impl std::io::Write, filename: &Path, text: &[u8]) -> std::io::Result<()> {
    let source_text = make_source_text(filename, text);
    let (mut root, _, state) = positioned_parser::parse_script(&source_text, Default::default());
    root.rewrite_pre_and_stop(|node| {
        match &mut node.syntax {
            // remove `<?hh` line if present
            SyntaxVariant::MarkupSuffix(..) => {
                *node = Syntax::make_missing(&state, 0);
                ControlFlow::Break(())
            }
            // remove function bodies, but not the line breaks after them: a
            // docblock directly after a body must start its own line to be
            // parsed as the next declaration's leading trivia
            SyntaxVariant::FunctionDeclaration(f) => {
                let semicolon = match f.function_body.trailing_token() {
                    Some(last) => Token::make(
                        TokenKind::Semicolon,
                        last.start_offset(),
                        last.width,
                        vec![],
                        last.trailing.clone(),
                    ),
                    None => Token::make(TokenKind::Semicolon, 0, 0, vec![], vec![]),
                };
                f.function_body = Syntax::make_token(semicolon);
                ControlFlow::Break(())
            }
            // remove method bodies
            SyntaxVariant::MethodishDeclaration(m) => {
                // no body
                m.methodish_function_body = Syntax::make_missing(&state, 0);
                // always have a semicolon, even if not abstract
                m.methodish_semicolon =
                    Syntax::make_token(Token::make(TokenKind::Semicolon, 0, 0, vec![], vec![]));
                ControlFlow::Break(())
            }
            // remove constant values
            SyntaxVariant::ConstantDeclarator(c) => {
                // no value
                c.constant_declarator_initializer = Syntax::make_missing(&state, 0);
                ControlFlow::Break(())
            }
            _ => ControlFlow::Continue(()),
        }
    });

    out.write_all(b"<?hh\n// @")?;
    out.write_all(b"generated from implementation\n\n")?;
    root.write_text_from_edited_tree(&source_text, out)?;
    out.write_all(b"\n")?;
    out.flush()?;

    Ok(())
}

/// Adds `@source <source_path>:<line>` to the docblock of each function in
/// `text`, creating a docblock for any function without one.
fn add_source_tags(filename: &Path, text: &[u8], source_path: &str) -> Vec<u8> {
    let source_text = make_source_text(filename, text);
    let indexed_source_text = IndexedSourceText::new(source_text.clone());
    let (root, _, _) = positioned_parser::parse_script(&source_text, Default::default());

    let mut edits = vec![];
    root.iter_pre(|node| {
        if let SyntaxVariant::FunctionDeclaration(f) = &node.syntax
            && let Some(first_token) = node.leading_token()
            && let Some(header_token) = f.function_declaration_header.leading_token()
        {
            let (line, _, _) =
                indexed_source_text.offset_to_file_pos_triple(header_token.start_offset());
            let tag = format!("@source {source_path}:{line}");
            edits.push(docblock_edit(text, first_token, &tag));
        }
    });
    edits.sort_by_key(|(range, _)| range.start);

    let mut annotated = Vec::with_capacity(text.len());
    let mut copied = 0;
    for (range, replacement) in edits {
        annotated.extend_from_slice(&text[copied..range.start]);
        annotated.extend_from_slice(&replacement);
        copied = range.end;
    }
    annotated.extend_from_slice(&text[copied..]);
    annotated
}

/// Returns how to add `tag` to the comment documenting `token`: the range of
/// `text` to replace, and its replacement. As in Hack's docblock finder, that
/// comment is the one directly before `token`, with no blank line between them.
/// If there is no such comment, `tag` gets a new docblock.
fn docblock_edit(text: &[u8], token: &Token, tag: &str) -> (Range<usize>, Vec<u8>) {
    let mut trivia = token
        .leading
        .iter()
        .rev()
        .filter(|trivium| trivium.kind != TriviaKind::WhiteSpace)
        .peekable();
    trivia.next_if(|trivium| trivium.kind == TriviaKind::EndOfLine);
    let comment = trivia.next();

    if let Some(comment) = comment
        && comment.kind == TriviaKind::DelimitedComment
        && let Some(body) = text[comment.offset..comment.offset + comment.width].strip_suffix(b"*/")
    {
        let indent = indentation(text, comment.offset);

        // put a one-line comment's text on its own line
        if !body.contains(&b'\n') {
            let opener: &[u8] = if body.starts_with(b"/**") {
                b"/**"
            } else {
                b"/*"
            };
            let replacement = [
                opener,
                format!("\n{indent} * ").as_bytes(),
                body[opener.len()..].trim_ascii(),
                format!("\n{indent} *\n{indent} * {tag}\n{indent} */").as_bytes(),
            ]
            .concat();
            return (comment.offset..comment.offset + comment.width, replacement);
        }

        let contents_end = body
            .iter()
            .rposition(|b| !b.is_ascii_whitespace())
            .map_or(0, |i| i + 1);
        let closing = if body[contents_end..].contains(&b'\n') {
            String::new()
        } else {
            format!("\n{indent}")
        };
        let offset = comment.offset + contents_end;
        return (
            offset..offset,
            format!("\n{indent} *\n{indent} * {tag}{closing}").into_bytes(),
        );
    }

    if let Some(comment) = comment
        && comment.kind == TriviaKind::SingleLineComment
    {
        let indent = indentation(text, comment.offset);
        let end = comment.offset + comment.width;
        return (end..end, format!("\n{indent}// {tag}").into_bytes());
    }

    let start = token.start_offset();
    let indent = indentation(text, start);
    (
        start..start,
        format!("/**\n{indent} * {tag}\n{indent} */\n{indent}").into_bytes(),
    )
}

/// Returns the whitespace before `offset` on its line, or nothing if anything
/// else precedes `offset` on that line.
fn indentation(text: &[u8], offset: usize) -> &str {
    let line_start = text[..offset]
        .iter()
        .rposition(|&b| b == b'\n')
        .map_or(0, |i| i + 1);
    let prefix = &text[line_start..offset];
    if prefix.iter().all(|&b| b == b' ' || b == b'\t') {
        std::str::from_utf8(prefix).expect("should be ASCII whitespace")
    } else {
        ""
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Split so that this file isn't marked as generated.
    const HEADER: &str = concat!("<?hh\n// @", "generated from implementation\n\n");

    fn hhi(text: &str, source_path: &str) -> String {
        let filename = Path::new("example.php");
        let text = add_source_tags(filename, text.as_bytes(), source_path);
        let mut out = vec![];
        write_hhi(&mut out, filename, &text).expect("writing to a Vec should succeed");
        String::from_utf8(out).expect("the HHI should be UTF-8")
    }

    const SOURCE: &str = r#"<?hh

namespace HH\Lib\Example;

/**
 * Has a docblock.
 */
<<__NoAutoLikes>>
function documented(): void {
  undocumented();
}

/** Has a one-line docblock. */
async function one_line(): Awaitable<void> {}

function undocumented(): void {}
/** Directly follows another function. */
function adjacent(): void {}

// Has a line comment.
function line_commented(): void {}

/** Documents nothing, since a blank line follows. */

function separated(): void {}

final class C {
  /**
   * Methods keep their docblocks.
   */
  public function method(): void {}
}
"#;

    #[test]
    fn test_source_tags() {
        let expected = r#"
namespace HH\Lib\Example;

/**
 * Has a docblock.
 *
 * @source hphp/example.php:9
 */
<<__NoAutoLikes>>
function documented(): void ;

/**
 * Has a one-line docblock.
 *
 * @source hphp/example.php:14
 */
async function one_line(): Awaitable<void> ;

/**
 * @source hphp/example.php:16
 */
function undocumented(): void ;
/**
 * Directly follows another function.
 *
 * @source hphp/example.php:18
 */
function adjacent(): void ;

// Has a line comment.
// @source hphp/example.php:21
function line_commented(): void ;

/** Documents nothing, since a blank line follows. */

/**
 * @source hphp/example.php:25
 */
function separated(): void ;

final class C {
  /**
   * Methods keep their docblocks.
   */
  public function method(): void ;}

"#;
        assert_eq!(
            hhi(SOURCE, "hphp/example.php"),
            format!("{HEADER}{expected}"),
            "each function should record the line of its `function` or `async` keyword, in the comment directly before it"
        );
    }
}
