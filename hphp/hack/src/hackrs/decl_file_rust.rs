// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::path::PathBuf;
use std::sync::Arc;

use clap::Parser;
use decl_parser::DeclParser;
use decl_parser::DeclParserOptions;
use folded_decl_provider::FoldedDeclProvider;
use hackrs_test_utils::decl_provider::make_folded_decl_provider;
use hackrs_test_utils::serde_store::StoreOpts;
use hackrs_test_utils::store::make_shallow_decl_store;
use hackrs_test_utils::store::populate_shallow_decl_store;
use jwalk::WalkDir;
use oxidized::decl_fold_options::DeclFoldOptions;
use pos::Prefix;
use pos::RelativePath;
use pos::RelativePathCtx;
use ty::decl::shallow;
use ty::reason::BReason;
use ty::reason::NReason;
use ty::reason::Reason;

#[derive(Parser, Debug)]
struct CliOptions {
    /// The Hack source files to declare.
    #[clap(value_name("FILEPATH"))]
    filenames: Vec<PathBuf>,

    /// The repository root (where .hhconfig is), e.g., ~/www
    #[clap(long)]
    root: Option<PathBuf>,

    /// Path to a SQLite naming table (allowing declaration of a single file in a large repo).
    #[clap(long)]
    naming_table: Option<PathBuf>,

    /// Allocate decls with positions instead of allocating position-free decls.
    #[clap(long)]
    with_pos: bool,

    /// Print the shallow decls in the file.
    #[clap(long)]
    shallow: bool,

    /// Print the folded decls of the classes in the file.
    #[clap(long)]
    folded: bool,
}

fn main() {
    let mut opts = CliOptions::parse();

    // If no modes were specified, print the shallow decls at least.
    if !opts.shallow && !opts.folded {
        opts.shallow = true;
    }

    if opts.with_pos {
        decl_files::<BReason>(&opts);
    } else {
        decl_files::<NReason>(&opts);
    }
}

fn decl_files<R: Reason>(opts: &CliOptions) {
    // Add hhi files to the given list of filenames
    let hhi_root = tempfile::TempDir::with_prefix("rupro_decl_file_hhi.").unwrap();
    hhi::write_hhi_files(hhi_root.path()).unwrap();

    let path_ctx = Arc::new(RelativePathCtx {
        root: opts.root.clone().unwrap_or_else(PathBuf::new),
        hhi: hhi_root.path().into(),
        dummy: PathBuf::new(),
        tmp: PathBuf::new(),
    });

    let filenames: Vec<RelativePath> = opts
        .filenames
        .iter()
        .map(|path| {
            if let Some(root) = opts.root.as_ref() {
                if let Ok(suffix) = path.strip_prefix(root) {
                    return RelativePath::new(Prefix::Root, suffix);
                }
            }
            RelativePath::new(Prefix::Dummy, path)
        })
        .collect();

    let mut all_filenames = WalkDir::new(&path_ctx.hhi)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| !e.file_type().is_dir())
        .map(|e| RelativePath::new(Prefix::Hhi, e.path().strip_prefix(&path_ctx.hhi).unwrap()))
        .collect::<Vec<_>>();
    let file_provider: Arc<dyn file_provider::FileProvider> =
        Arc::new(file_provider::DiskProvider::new(path_ctx, Some(hhi_root)));
    let decl_parser =
        DeclParser::<R>::new(Arc::clone(&file_provider), DeclParserOptions::default());
    all_filenames.extend(&filenames);

    let shallow_decl_store = make_shallow_decl_store(StoreOpts::Unserialized);
    populate_shallow_decl_store(&shallow_decl_store, decl_parser.clone(), &all_filenames);

    let folded_decl_provider = Arc::new(make_folded_decl_provider(
        StoreOpts::Unserialized,
        opts.naming_table.as_ref(),
        shallow_decl_store,
        Arc::new(DeclFoldOptions::default()),
        decl_parser.clone(),
    ));

    let mut saw_err = false;

    for path in filenames {
        for decl in decl_parser.parse(path).unwrap() {
            match decl {
                shallow::NamedDecl::Class(name, decl) => {
                    if opts.shallow {
                        println!("{:#?}", decl);
                    }
                    if opts.folded {
                        match folded_decl_provider.get_class(name) {
                            Ok(decl) => println!(
                                "{:#?}",
                                decl.expect("expected decl provider to return Some")
                            ),
                            Err(e) => {
                                saw_err = true;
                                eprintln!("Error: {}", e);
                            }
                        }
                    }
                }
                shallow::NamedDecl::Fun(_, decl) => println!("{:#?}", decl),
                shallow::NamedDecl::Typedef(_, decl) => println!("{:#?}", decl),
                shallow::NamedDecl::Const(_, decl) => println!("{:#?}", decl),
                shallow::NamedDecl::Module(_, decl) => println!("{:#?}", decl),
            }
        }
    }

    if saw_err {
        std::process::exit(1);
    }
}
