// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Expr_;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct GuardInvalidPass;

impl Pass for GuardInvalidPass {
    fn on_ty_expr__top_down(&mut self, _: &Env, elem: &mut Expr_) -> ControlFlow<()> {
        if matches!(elem, Expr_::Invalid(..)) {
            Break(())
        } else {
            Continue(())
        }
    }
}

#[cfg(test)]
mod tests {

    use nast::Binop;
    use nast::Bop;
    use nast::Expr;
    use nast::Expr_;
    use nast::Pos;

    use super::*;

    #[derive(Clone)]
    pub struct RewriteZero;
    impl Pass for RewriteZero {
        fn on_ty_expr__bottom_up(&mut self, _: &Env, elem: &mut Expr_) -> ControlFlow<()> {
            match elem {
                Expr_::Int(..) => *elem = Expr_::Int("0".to_string()),
                _ => (),
            }
            Continue(())
        }
    }

    #[test]
    fn test() {
        let env = Env::default();

        let mut pass = crate::pass::Passes {
            passes: vec![Box::new(GuardInvalidPass), Box::new(RewriteZero)],
        };

        let mut elem = Expr_::Binop(Box::new(Binop {
            bop: Bop::Lt,
            lhs: Expr(
                (),
                Pos::NONE,
                Expr_::Invalid(Box::new(Some(Expr(
                    (),
                    Pos::NONE,
                    Expr_::Int("42".to_string()),
                )))),
            ),
            rhs: Expr((), Pos::NONE, Expr_::Int("43".to_string())),
        }));

        elem.transform(&env, &mut pass);

        assert!(matches!(
                elem,
                Expr_::Binop(deref!(Binop{
                    lhs:Expr(_, _, Expr_::Invalid(deref!(Some(Expr(_, _, Expr_::Int(n)))))),
                    rhs:Expr(_, _, Expr_::Int(m)),
                    ..
        }))
                if n == "42" && m == "0"
            ));
    }
}
