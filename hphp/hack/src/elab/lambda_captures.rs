// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! Walk the NAST, track free variables, and add them to capture lists in
//! lambdas.
//!
//! A free variable is any local that isn't bound as a parameter or directly
//! defined.
//!
//! ```notrust
//!     ($a) ==> {
//!       $b = $a;
//!       $c;
//!     }
//! ```
//!
//! In this example, only $c is free.

use std::collections::BTreeMap;

use naming_special_names_rust as sn;
use nast::CaptureLid;
use nast::Expr;
use nast::Expr_;
use nast::Lid;
use nast::LocalId;
use nast::Pos;
use nast::ShapeExprField;
use oxidized::aast_visitor::AstParams;
use oxidized::aast_visitor::NodeMut;
use oxidized::aast_visitor::VisitorMut;
use oxidized::local_id;
use oxidized::naming_error::NamingError;
use oxidized::nast;

use crate::env::Env;

#[derive(Clone, Default)]
pub struct Visitor {
    bound: BTreeMap<LocalId, Pos>,
    free: BTreeMap<LocalId, Pos>,
}

impl Visitor {
    fn add_local_def(&mut self, lid: Lid) {
        self.bound.insert(lid.1, lid.0);
    }
    fn add_local_defs(&mut self, lids: impl Iterator<Item = CaptureLid>) {
        for capture_lid in lids {
            let CaptureLid(_, lid) = capture_lid;
            self.add_local_def(lid);
        }
    }

    fn add_param(&mut self, param: &nast::FunParam) {
        self.add_local_def(Lid(param.pos.clone(), local_id::make_unscoped(&param.name)));
    }
    fn add_params(&mut self, f: &nast::Fun_) {
        for param in &f.params {
            self.add_param(param);
        }
    }

    fn add_local_defs_from_lvalue(&mut self, Expr(_, _, e): &Expr) {
        match e {
            Expr_::List(lv) | Expr_::Tuple(lv) => {
                lv.iter().for_each(|e| self.add_local_defs_from_lvalue(e))
            }
            Expr_::Shape(lv) => lv.iter().for_each(|f| match f {
                ShapeExprField::SFField(_, e) => self.add_local_defs_from_lvalue(e),
                ShapeExprField::SFSplat(_) => {}
            }),
            Expr_::Lvar(deref!(lid)) => self.add_local_def(lid.clone()),
            _ => {}
        }
    }

    fn add_local_ref(&mut self, lid: &Lid) {
        let local_id = lid.as_local_id();
        if !self.bound.contains_key(local_id) {
            self.free.insert(local_id.clone(), lid.pos().clone());
        }
    }
}

impl<'ast> VisitorMut<'ast> for Visitor {
    type Params = AstParams<Env, ()>;

    fn object(&mut self) -> &mut dyn VisitorMut<'ast, Params = Self::Params> {
        self
    }

    fn visit_expr_(&mut self, env: &mut Env, e: &mut Expr_) -> Result<(), ()> {
        match e {
            Expr_::Lvar(lv) => {
                self.add_local_ref(lv);
                e.recurse(env, self.object())
            }
            Expr_::Assign(deref!((lhs, _, _))) => {
                // Introducing a new local variable.
                //
                //     $x = ...
                self.add_local_defs_from_lvalue(lhs);
                e.recurse(env, self.object())
            }
            Expr_::Lfun(deref!((f, idl))) => {
                let outer_vars = std::mem::take(self);

                // We want to know about free variables inside the lambda, but
                // we don't want its bound variables.
                self.add_params(f);
                f.recurse(env, self.object())?;
                let inner_free = std::mem::take(&mut self.free);
                *idl = inner_free
                    .iter()
                    .rev()
                    .map(|(lid, pos)| CaptureLid((), Lid(pos.clone(), lid.clone())))
                    .collect();
                *self = outer_vars;
                self.free.extend(inner_free);
                Ok(())
            }
            _ => e.recurse(env, self.object()),
        }
    }

    fn visit_as_expr(&mut self, env: &mut Env, ae: &mut nast::AsExpr) -> Result<(), ()> {
        // `as` inside a foreach loop introduces a new local variable.
        //
        //     foreach(... as $x) { ... }
        match ae {
            nast::AsExpr::AsV(e) | nast::AsExpr::AwaitAsV(_, e) => {
                self.add_local_defs_from_lvalue(e);
            }
            nast::AsExpr::AsKv(k, v) | nast::AsExpr::AwaitAsKv(_, k, v) => {
                self.add_local_defs_from_lvalue(k);
                self.add_local_defs_from_lvalue(v);
            }
        }
        ae.recurse(env, self.object())
    }

    fn visit_stmt_(&mut self, env: &mut Env, s: &mut nast::Stmt_) -> Result<(), ()> {
        // `concurrent` blocks are desugared to a list of expressions,
        // which can introduce new locals.
        //
        //     concurrent {
        //       $x = await foo();
        //       await bar();
        //     }
        if let nast::Stmt_::Awaitall(deref!((el, _))) = s {
            for (lid, _) in el {
                self.add_local_def(lid.clone())
            }
        }
        s.recurse(env, self.object())
    }

    fn visit_catch(&mut self, env: &mut Env, c: &mut nast::Catch) -> Result<(), ()> {
        // `catch` introduces a new local variable.
        //
        //     try { ... } catch (Foo $x) { ... } *)
        let nast::Catch(_, lv, _) = c;
        self.add_local_def(lv.clone());
        c.recurse(env, self.object())
    }

    fn visit_efun(&mut self, env: &mut Env, efun: &mut nast::Efun) -> Result<(), ()> {
        let outer_vars = std::mem::take(self);
        let idl = efun.use_.clone();

        // We want to know about free variables inside the lambda, but
        // we don't want its bound variables.
        self.add_params(&efun.fun);
        self.add_local_defs(idl.iter().cloned());
        efun.recurse(env, self.object())?;
        let inner_free = std::mem::take(&mut self.free);
        *self = outer_vars;
        self.free.extend(inner_free);

        // Efun syntax requires that the user specifies the captures.
        //
        //     function() use($captured1, $captured2) { ... }
        //
        // We just check that they haven't tried to explicitly capture $this.
        let idl = idl
            .into_iter()
            .filter(|CaptureLid(_, Lid(p, lid))| {
                let is_this = local_id::get_name(lid) == sn::special_idents::THIS;
                if is_this {
                    env.emit_error(NamingError::ThisAsLexicalVariable(p.clone()));
                }
                !is_this
            })
            .collect();
        efun.use_ = idl;
        Ok(())
    }
}

pub fn elaborate_program(env: &mut Env, program: &mut nast::Program) {
    Visitor::default().visit_program(env, program).unwrap();
}

pub fn elaborate_fun_def(env: &mut Env, fd: &mut nast::FunDef) {
    Visitor::default().visit_fun_def(env, fd).unwrap();
}

pub fn elaborate_class_(env: &mut Env, c: &mut nast::Class_) {
    Visitor::default().visit_class_(env, c).unwrap();
}

pub fn elaborate_module_def(env: &mut Env, m: &mut nast::ModuleDef) {
    Visitor::default().visit_module_def(env, m).unwrap();
}

pub fn elaborate_gconst(env: &mut Env, cst: &mut nast::Gconst) {
    Visitor::default().visit_gconst(env, cst).unwrap();
}

pub fn elaborate_typedef(env: &mut Env, td: &mut nast::Typedef) {
    Visitor::default().visit_typedef(env, td).unwrap();
}

pub fn elaborate_stmt(env: &mut Env, stmt: &mut nast::Stmt) {
    Visitor::default().visit_stmt(env, stmt).unwrap();
}
