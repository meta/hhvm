// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

/// A `lex`/`yacc` like parser generator tool built around
/// [softdevteam/grmtools](https://github.com/softdevteam/grmtools).
#[derive(Debug, clap::Parser)]
struct Opts {
    /// Lexer definition ('.l' file)
    #[clap(short, long)]
    lexer: std::path::PathBuf,

    /// Parser definition ('.y' file)
    #[clap(short, long)]
    parser: std::path::PathBuf,

    /// The output directory
    #[clap(short, long)]
    outdir: std::path::PathBuf,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut opts = <Opts as clap::Parser>::parse();

    let lexer_in = std::mem::take(&mut opts.lexer);
    let parser_in = std::mem::take(&mut opts.parser);
    let outdir_in = std::mem::take(&mut opts.outdir);
    let outdir_out = outdir_in.display();
    let lexer_out = format!(
        "{}/{}_l.rs",
        outdir_out,
        lexer_in.file_stem().unwrap().to_str().unwrap()
    );
    let parser_out = format!(
        "{}/{}_y.rs",
        outdir_out,
        parser_in.file_stem().unwrap().to_str().unwrap()
    );

    lrlex::CTLexerBuilder::<lrlex::DefaultLexerTypes<u32>>::new()
        .lrpar_config(move |ctp| {
            ctp.yacckind(cfgrammar::yacc::YaccKind::Grmtools)
                .grammar_path(parser_in.clone())
                .output_path(parser_out.clone())
        })
        .lexer_path(lexer_in)
        .output_path(lexer_out)
        .build()?;

    Ok(())
}
