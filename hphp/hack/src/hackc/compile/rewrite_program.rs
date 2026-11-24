// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
#![feature(deref_patterns)]

use std::path::PathBuf;
use std::sync::Arc;

use env::emitter::Emitter;
use error::Error;
use error::Result;
use hack_macros::hack_expr;
use hack_macros::hack_stmt;
use naming_special_names_rust::modules as special_modules;
use oxidized::ast;
use oxidized::ast::Def;
use oxidized::ast::Expr;
use oxidized::ast::Expr_;
use oxidized::ast::Fun_;
use oxidized::ast::FunDef;
use oxidized::ast::FunKind;
use oxidized::ast::FuncBody;
use oxidized::ast::Id;
use oxidized::ast::Pos;
use oxidized::ast::Stmt;
use oxidized::ast::Stmt_;
use oxidized::ast::TypeHint;
use oxidized::ast::UserAttribute;
use oxidized::file_info::Mode;
use oxidized::local_id;
use oxidized::namespace_env;
use relative_path::Prefix;
use relative_path::RelativePath;
use rewrite_xml::rewrite_xml;

fn debugger_eval_should_modify(tast: &[ast::Def]) -> Result<bool> {
    /*
    The AST currently always starts with a Markup statement, so a
    length of 2 means there was 1 user def (statement, function,
    etc.); we assert that the first thing is a Markup statement, and
    we only want to modify if there was exactly one user def (both 0
    user defs and > 1 user def are valid situations where we pass the
    program through unmodififed)
    */
    if tast
        .first()
        .and_then(|x| x.as_stmt())
        .is_none_or(|x| !x.1.is_markup())
    {
        return Err(Error::unrecoverable(
            "Lowered AST did not start with a Markup statement",
        ));
    }

    if tast.len() != 2 {
        Ok(false)
    } else {
        Ok(tast[1].as_stmt().is_some_and(|x| x.1.is_expr()))
    }
}

pub fn rewrite_program(
    emitter: &mut Emitter,
    prog: &mut ast::Program,
    namespace_env: Arc<namespace_env::Env>,
) -> Result<()> {
    let for_debugger_eval =
        emitter.for_debugger_eval && debugger_eval_should_modify(prog.as_slice())?;
    if !emitter.for_debugger_eval {
        let contains_toplevel_code = prog.iter().find_map(|d| {
            if let Some(ast::Stmt(pos, s_)) = d.as_stmt() {
                if s_.is_markup() { None } else { Some(pos) }
            } else {
                None
            }
        });
        if let Some(pos) = contains_toplevel_code {
            return Err(Error::fatal_parse(pos, "Found top-level code"));
        }
    }

    if emitter.options().compiler_flags.constant_folding {
        constant_folder::fold_program(prog, emitter)
            .map_err(|e| Error::unrecoverable(format!("{}", e)))?;
    }

    if emitter.for_debugger_eval {
        extract_debugger_main(&namespace_env, &mut prog.0).map_err(Error::unrecoverable)?;
    }

    // TODO: wind through flags.disable_toplevel_elaboration?
    // This `flatten_ns` is not currently needed because unless
    // `--disable-toplevel-elaboration` is set, we already do this while
    // parsing.  We may want to move that functionality into elab and remove
    // this flattening (and remove it from the parser?)
    if true {
        prog.0 = flatten_ns(prog.0.drain(..));
    }

    closure_convert::convert_toplevel_prog(emitter, &mut prog.0, namespace_env)?;

    emitter.for_debugger_eval = for_debugger_eval;

    rewrite_xml(emitter, prog)?;

    Ok(())
}

fn update_awaitall(stmts: &mut [Stmt], return_val: &ast::LocalId) {
    match stmts.last_mut() {
        Some(Stmt(_, Stmt_::Block(deref!((_, ast::Block(stmts)))))) => {
            update_awaitall(stmts, return_val);
        }
        Some(Stmt(_, Stmt_::Awaitall(deref!((_, block))))) => match block.last_mut() {
            Some(s) => match s {
                Stmt(_, Stmt_::Expr(deref!(Expr(_, p, e)))) => {
                    let e_inner = Expr((), std::mem::take(p), std::mem::replace(e, Expr_::False));
                    *s = hack_stmt!("#{lvar(clone(return_val))} = #e_inner;");
                }
                _ => {}
            },
            _ => {}
        },
        _ => {}
    }
}

/// The function we emit for debugger takes all variables used in the block of
/// code as parameters to the function created and returns the updated version
/// of these variables as a vector, placing the result of executing this function
/// as the 0th index of this vector.
fn extract_debugger_main(
    empty_namespace: &Arc<namespace_env::Env>,
    all_defs: &mut Vec<Def>,
) -> std::result::Result<(), String> {
    let (mut stmts, mut defs): (Vec<Def>, Vec<Def>) = all_defs.drain(..).partition(|x| x.is_stmt());
    let mut vars = decl_vars::vars_from_ast(&[], &stmts, true)?
        .into_iter()
        .collect::<Vec<_>>();

    // In order to find the return value of these sets of statements, we must
    // search and obtain the "return statement".
    // This basic pass looks at all the "return statements" and
    // then replaces this stament with a variable set, so that we can later
    // add this variable into our return result.
    use oxidized::aast_visitor::NodeMut;
    use oxidized::aast_visitor::VisitorMut;
    struct Ctx {
        return_val: local_id::LocalId,
    }
    struct Visitor {}
    impl<'node> VisitorMut<'node> for Visitor {
        type Params = oxidized::aast_visitor::AstParams<Ctx, Error>;
        fn object(&mut self) -> &mut dyn VisitorMut<'node, Params = Self::Params> {
            self
        }

        fn visit_stmt(&mut self, env: &mut Ctx, e: &'node mut ast::Stmt) -> Result<()> {
            match e {
                Stmt(p, Stmt_::Return(deref!(value @ Some(_)))) => {
                    let value = value.take();
                    let return_val = &env.return_val;
                    let expr: Expr = value.unwrap();
                    *e = hack_stmt!(pos = p.clone(), "#{lvar(clone(return_val))} = #expr;");
                    Ok(())
                }
                _ => e.recurse(env, self),
            }
        }

        fn visit_expr(&mut self, env: &mut Ctx, e: &'node mut ast::Expr) -> Result<()> {
            match &e.2 {
                // Do not recurse into closures
                Expr_::Lfun(_) | Expr_::Efun(_) => Ok(()),
                _ => e.recurse(env, self),
            }
        }
    }

    let mut ctx = Ctx {
        return_val: local_id::make_unscoped("$__debugger$return_val"),
    };
    for stmt in &mut stmts {
        oxidized::aast_visitor::visit_mut(&mut Visitor {}, &mut ctx, stmt).unwrap();
    }
    let return_val = ctx.return_val;

    let mut stmts = stmts
        .into_iter()
        .filter_map(|x| x.as_stmt_into())
        .collect::<Vec<_>>();

    let is_empty = |defs: &[Def]| {
        defs.iter()
            .filter(|def| match def {
                // Ignore the default module that was artificially inserted in the lowerer
                Def::SetModule(deref!(Id(_, md))) if md == special_modules::DEFAULT => false,
                _ => true,
            })
            .count()
            == 0
    };
    if is_empty(&defs) && stmts.len() == 2 && stmts[0].1.is_markup() && stmts[1].1.is_expr() {
        let Stmt(p, s) = stmts.pop().unwrap();
        let e = s.as_expr_into().unwrap();
        stmts.push(hack_stmt!(pos = p, "#{lvar(clone(return_val))} = #e;"));
    }

    update_awaitall(&mut stmts, &return_val);

    let p = || Pos::NONE;
    let mut unsets: ast::Block = vars
        .iter()
        .map(|name| {
            let name = local_id::make_unscoped(name);
            hack_stmt!(
                r#"if (#{lvar(clone(name))} is __uninitSentinel) {
                        \__SystemLib\__debugger_make_uninit(#{lvar(name)});
                     }
                "#
            )
        })
        .collect();
    let sets: Vec<_> = vars
        .iter()
        .map(|name: &String| {
            let name = local_id::make_unscoped(name);
            hack_stmt!(
                pos = p(),
                r#"if (\__SystemLib\__debugger_is_uninit(#{lvar(clone(name))})) {
                        #{lvar(name)} = new __uninitSentinel();
                     }
                "#
            )
        })
        .collect();
    vars.push("$__debugger$this".into());
    vars.push("$__debugger_exn$output".into());
    let (params, return_val_sets): (Vec<_>, Vec<_>) = vars
        .iter()
        .map(|var| {
            let name = local_id::make_unscoped(var);
            let param = closure_convert::make_fn_param(p(), &name, false, false);
            let var = hack_expr!(pos = p(), r#"#{lvar(name)}"#);
            (param, var)
        })
        .unzip();
    let exnvar = local_id::make_unscoped("$__debugger_exn$output");
    unsets.push(hack_stmt!("#{lvar(clone(return_val))} = null;"));
    let catch = hack_stmt!(
        pos = p(),
        r#"
            try {
              #{stmts*};
            } catch (Throwable #{lvar(exnvar)}) {
              /* no-op */
            } finally {
              #{sets*};
              return vec[#{lvar(return_val)}, #{return_val_sets*}];
            }
        "#
    );
    unsets.push(catch);
    let body = unsets;
    let pos = Pos::from_line_cols_offset(
        Arc::new(RelativePath::make(Prefix::Dummy, PathBuf::from(""))),
        1,
        0..0,
        0,
    );
    let f = Fun_ {
        span: pos,
        annotation: (),
        readonly_this: None, // TODO(readonly): readonly_this in closure_convert
        readonly_ret: None,  // TODO(readonly): readonly_ret in closure_convert
        tparams: vec![],
        ret: TypeHint((), None),
        params,
        ctxs: None,        // TODO(T70095684)
        unsafe_ctxs: None, // TODO(T70095684)
        body: FuncBody { fb_ast: body },
        fun_kind: FunKind::FAsync,
        user_attributes: ast::UserAttributes(vec![UserAttribute {
            name: Id(Pos::NONE, "__DebuggerMain".into()),
            params: vec![],
        }]),
        external: false,
        hidden: false,
        doc_comment: None,
    };
    let fd = FunDef {
        namespace: Arc::clone(empty_namespace),
        file_attributes: vec![],
        mode: Mode::Mstrict,
        name: Id(Pos::NONE, "include".into()),
        fun: f,
        // TODO(T116039119): Populate value with presence of internal attribute
        internal: false,
        module: None,
        tparams: vec![],
        where_constraints: vec![],
        package: None,
    };
    let mut new_defs = vec![Def::mk_fun(fd)];
    new_defs.append(&mut defs);
    *all_defs = new_defs;
    Ok(())
}

fn flatten_ns(defs: impl ExactSizeIterator<Item = Def>) -> Vec<Def> {
    fn helper(out: &mut Vec<Def>, defs: impl ExactSizeIterator<Item = Def>) {
        out.reserve(defs.len());
        for def in defs {
            match def {
                Def::Namespace(ns) => {
                    helper(out, ns.1.into_iter());
                }
                _ => out.push(def),
            }
        }
    }

    let mut out = Vec::with_capacity(defs.len());
    helper(&mut out, defs);
    out
}
