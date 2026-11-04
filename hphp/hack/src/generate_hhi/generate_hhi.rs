// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::path::PathBuf;

/// Convert a Hack source file to an HHI interface definition file by removing
/// all function and method bodies.
#[derive(clap::Parser, Debug)]
struct Opts {
    /// The Hack source file to generate an HHI file for.
    filename: PathBuf,

    /// Record where each function is implemented in its docblock, as
    /// `@source <SOURCE_PATH>:<line>`.
    #[clap(long)]
    source_path: Option<String>,
}

fn main() -> anyhow::Result<()> {
    let opts = <Opts as clap::Parser>::parse();
    let mut stdout = std::io::BufWriter::new(std::io::stdout().lock());
    generate_hhi_lib::run(&mut stdout, &opts.filename, opts.source_path.as_deref())
}
