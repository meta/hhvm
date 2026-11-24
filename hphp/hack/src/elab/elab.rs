// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

#![feature(deref_patterns)]

mod elab_utils;
mod env;
mod lambda_captures;
mod lift_await;
mod pass;
mod passes;
mod transform;

/// Private convenience module for simplifying imports in pass implementations.
mod prelude {
    pub use std::ops::ControlFlow;
    pub use std::ops::ControlFlow::Break;
    pub use std::ops::ControlFlow::Continue;

    pub use naming_special_names_rust as sn;
    pub use oxidized::nast;

    pub use crate::env::Env;
    pub use crate::pass::Pass;
    #[allow(unused_imports)]
    pub use crate::transform::Transform;
}

use std::path::Path;
use std::sync::Arc;

use env::Env;
use env::ProgramSpecificOptions;
use hash::HashSet;
use oxidized::experimental_features::FeatureName;
use oxidized::namespace_env;
use oxidized::namespace_env::Mode;
use oxidized::naming_phase_error::NamingPhaseError;
use oxidized::nast;
use oxidized::typechecker_options::TypecheckerOptions;
use pass::Pass;
use relative_path::RelativePath;
use transform::Transform;
use vec1::Vec1;

/// Provided for use in hackc as a simple collection of knobs outside of namespace env.
#[derive(Clone, Debug, Default)]
pub struct CodegenOpts {
    pub textual_remove_memoize: bool,
    pub emit_checked_unsafe_cast: bool,
}

/// Provided for use in hackc, where we have an `ns_env` in hand already.
/// Expected to behave the same as `elaborate_program` when `po_codegen` is
/// `true`.
pub fn elaborate_program_for_codegen(
    ns_env: Arc<namespace_env::Env>,
    path: &RelativePath,
    program: &mut nast::Program,
    opts: &CodegenOpts,
    active_experimental_features: HashSet<FeatureName>,
) -> Result<(), Vec1<NamingPhaseError>> {
    assert!(matches!(ns_env.mode, Mode::ForCodegen));
    let po = oxidized::parser_options::ParserOptions {
        codegen: true,
        disable_xhp_element_mangling: ns_env.disable_xhp_element_mangling,
        // Do not copy the auto_ns_map; it's not read in this crate except via
        // elaborate_namespaces_visitor, which uses the one in `ns_env` here
        ..Default::default()
    };
    let tco = TypecheckerOptions {
        po,
        ..Default::default()
    };
    elaborate_namespaces_visitor::elaborate_program(ns_env, program);
    let mut env = make_env(&tco, path, active_experimental_features);
    elaborate_common(&env, program);
    elaborate_package_expr(&env, program);
    elaborate_for_codegen(&env, program, opts);
    // Passes below here can emit errors
    lift_await::elaborate_program(&mut env, program, tco.po.codegen);
    let errs = env.into_errors();
    match Vec1::try_from_vec(errs) {
        Err(_) => Ok(()),
        Ok(v) => Err(v),
    }
}

pub fn elaborate_program(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    program: &mut nast::Program,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_program(ns_env(tco), program);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, program);
    if !tco.po.codegen {
        lambda_captures::elaborate_program(&mut env, program);
        lift_await::elaborate_program(&mut env, program, false);
    }
    env.into_errors()
}

pub fn elaborate_fun_def(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    f: &mut nast::FunDef,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_fun_def(ns_env(tco), f);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, f);
    if !tco.po.codegen {
        lambda_captures::elaborate_fun_def(&mut env, f);
        lift_await::elaborate_fun_def(&mut env, f, false);
    }
    env.into_errors()
}

pub fn elaborate_class_(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    c: &mut nast::Class_,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_class_(ns_env(tco), c);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, c);
    if !tco.po.codegen {
        lambda_captures::elaborate_class_(&mut env, c);
        lift_await::elaborate_class_(&mut env, c, false);
    }
    env.into_errors()
}

pub fn elaborate_module_def(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    m: &mut nast::ModuleDef,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_module_def(ns_env(tco), m);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, m);
    if !tco.po.codegen {
        lambda_captures::elaborate_module_def(&mut env, m);
    }
    env.into_errors()
}

pub fn elaborate_gconst(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    c: &mut nast::Gconst,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_gconst(ns_env(tco), c);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, c);
    if !tco.po.codegen {
        lambda_captures::elaborate_gconst(&mut env, c);
    }
    env.into_errors()
}

pub fn elaborate_typedef(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    t: &mut nast::Typedef,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_typedef(ns_env(tco), t);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, t);
    if !tco.po.codegen {
        lambda_captures::elaborate_typedef(&mut env, t);
    }
    env.into_errors()
}

pub fn elaborate_stmt(
    tco: &TypecheckerOptions,
    path: &RelativePath,
    t: &mut nast::Stmt,
    active_experimental_features: HashSet<FeatureName>,
) -> Vec<NamingPhaseError> {
    elaborate_namespaces_visitor::elaborate_stmt(ns_env(tco), t);
    let mut env = make_env(tco, path, active_experimental_features);
    elaborate_common(&env, t);
    if !tco.po.codegen {
        lambda_captures::elaborate_stmt(&mut env, t);
        lift_await::elaborate_stmt(&mut env, t, false);
    }
    env.into_errors()
}

fn ns_env(tco: &oxidized::global_options::GlobalOptions) -> Arc<namespace_env::Env> {
    Arc::new(namespace_env::Env::empty(
        tco.po.auto_namespace_map.clone(),
        if tco.po.codegen {
            Mode::ForCodegen
        } else {
            Mode::ForTypecheck
        },
        tco.po.disable_xhp_element_mangling,
    ))
}

fn filename_in_allowed(allowed_files: &[String], path: &Path) -> bool {
    allowed_files.iter().any(|spec| {
        !spec.is_empty()
            && (spec.ends_with('*') && path.starts_with(&spec[..spec.len() - 1])
                || path == std::path::Path::new(spec))
    })
}

fn make_env(
    tco: &TypecheckerOptions,
    rel_path: &RelativePath,
    active_experimental_features: HashSet<FeatureName>,
) -> Env {
    let is_hhi = rel_path.is_hhi();
    let path = rel_path.path();

    let allow_module_declarations = tco.tco_allow_all_files_for_module_declarations
        || filename_in_allowed(&tco.tco_allowed_files_for_module_declarations, path);

    let allow_ignore_readonly =
        filename_in_allowed(&tco.tco_allowed_files_for_ignore_readonly, path);

    Env::new(
        tco,
        &ProgramSpecificOptions {
            is_hhi,
            allow_module_declarations,
            allow_ignore_readonly,
        },
        active_experimental_features,
    )
}

/// Run the passes which are common to codegen and typechecking.
/// For now, these passes may not emit errors.
fn elaborate_common<T: Transform>(_env: &Env, _node: &mut T) {
    // Shape/tuple destructure disambiguation is now handled in the lowerer
    // (ExprLocation::LvaluePosition), so no common passes remain.
}

fn elaborate_package_expr<T: Transform>(env: &Env, node: &mut T) {
    #[rustfmt::skip]
    let mut passes = pass::Passes { passes: vec![
            Box::<passes::elab_expr_package::ElabExprPackagePass>::default()
    ]};
    node.transform(env, &mut passes);
    env.assert_no_errors();
}

fn elaborate_for_codegen<T: Transform>(env: &Env, node: &mut T, opts: &CodegenOpts) {
    let mut passes: Vec<Box<dyn Pass>> = Vec::new();

    if opts.textual_remove_memoize {
        passes.push(Box::<passes::remove_memo_attr::RemoveMemoAttr>::default());
    }
    // Insert assertion that the package being require-package accessed is loaded
    passes.push(Box::<passes::elab_require_package::ElabRequirePackagePass>::default());

    passes.push(Box::new(passes::drop_unsafe_cast::DropUnsafeCast {
        emit_checked_unsafe_cast: opts.emit_checked_unsafe_cast,
    }));

    let mut passes = pass::Passes { passes };
    node.transform(env, &mut passes);
    env.assert_no_errors();
}
