// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::AsExpr;
use nast::Expr;
use nast::Expr_;
use nast::Lid;
use oxidized::local_id;

use crate::prelude::*;

#[derive(Copy, Clone, Default)]
pub struct ElabAsExprPass;

impl Pass for ElabAsExprPass {
    fn on_ty_as_expr_bottom_up(&mut self, env: &Env, elem: &mut AsExpr) -> ControlFlow<()> {
        match elem {
            AsExpr::AsV(e) | AsExpr::AwaitAsV(_, e) => elab_value(env, e),
            AsExpr::AsKv(ek, ev) | AsExpr::AwaitAsKv(_, ek, ev) => {
                elab_key(env, ek);
                elab_value(env, ev);
            }
        }
        Continue(())
    }
}

fn elab_value(env: &Env, expr: &mut Expr) {
    let Expr(_, pos, expr_) = expr;
    if matches!(expr_, Expr_::Id(..)) {
        env.emit_error(NamingError::ExpectedVariable(pos.clone()));
        *expr_ = Expr_::Lvar(Box::new(Lid(
            pos.clone(),
            local_id::make_unscoped("__internal_placeholder"),
        )))
    }
}

fn elab_key(env: &Env, expr: &mut Expr) {
    let Expr(_, pos, expr_) = expr;
    match expr_ {
        Expr_::Lvar(..) | Expr_::Lplaceholder(..) => (),
        _ => {
            env.emit_error(NamingError::ExpectedVariable(pos.clone()));
            *expr_ = Expr_::Lvar(Box::new(Lid(
                pos.clone(),
                local_id::make_unscoped("__internal_placeholder"),
            )))
        }
    }
}

#[cfg(test)]
mod tests {

    use nast::Id;
    use nast::Pos;

    use super::*;

    #[test]
    fn test_value_invalid() {
        let env = Env::default();

        let mut pass = ElabAsExprPass;

        let mut elem = AsExpr::AsV(Expr(
            (),
            Pos::default(),
            Expr_::Id(Box::new(Id(Pos::default(), String::default()))),
        ));
        elem.transform(&env, &mut pass);

        assert!(matches!(
            env.into_errors().as_slice(),
            [NamingPhaseError::Naming(NamingError::ExpectedVariable(..))]
        ));
        assert!(match elem {
            AsExpr::AsV(Expr(_, _, Expr_::Lvar(deref!(Lid(_, lid))))) =>
                lid.1 == "__internal_placeholder",
            _ => false,
        })
    }

    #[test]
    fn test_value_valid() {
        let env = Env::default();

        let mut pass = ElabAsExprPass;

        let mut elem = AsExpr::AsV(elab_utils::expr::null());
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(elem, AsExpr::AsV(Expr(_, _, Expr_::Null))))
    }

    #[test]
    fn test_key_invalid() {
        let env = Env::default();

        let mut pass = ElabAsExprPass;

        let mut elem = AsExpr::AsKv(elab_utils::expr::null(), elab_utils::expr::null());
        elem.transform(&env, &mut pass);

        assert!(matches!(
            env.into_errors().as_slice(),
            [NamingPhaseError::Naming(NamingError::ExpectedVariable(..))]
        ));
        assert!(match elem {
            AsExpr::AsKv(Expr(_, _, Expr_::Lvar(deref!(Lid(_, lid)))), Expr(_, _, Expr_::Null)) =>
                lid.1 == "__internal_placeholder",
            _ => false,
        })
    }

    #[test]
    fn test_key_valid() {
        let env = Env::default();

        let mut pass = ElabAsExprPass;

        let mut elem = AsExpr::AsKv(
            Expr((), Pos::default(), Expr_::Lplaceholder(Box::default())),
            elab_utils::expr::null(),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(
            elem,
            AsExpr::AsKv(Expr(_, _, Expr_::Lplaceholder(..)), Expr(_, _, Expr_::Null))
        ))
    }
}
