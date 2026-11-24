// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Expr_;
use nast::Hint;
use nast::Hint_;
use nast::Id;
use nast::Tprim::*;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ValidateExprCastPass;

impl Pass for ValidateExprCastPass {
    fn on_ty_expr__bottom_up(&mut self, env: &Env, expr: &mut Expr_) -> ControlFlow<()> {
        match &*expr {
            Expr_::Cast(
                deref!((
                    Hint(_, deref!(Hint_::Hprim(Tint | Tbool | Tfloat | Tstring))),
                    _
                )),
            ) => Continue(()),
            Expr_::Cast(deref!((Hint(_, deref!(Hint_::Happly(Id(_, tycon_nm), _))), _)))
                if tycon_nm == sn::collections::DICT || tycon_nm == sn::collections::VEC =>
            {
                Continue(())
            }
            Expr_::Cast(deref!((Hint(_, deref!(Hint_::HvecOrDict(_, _))), _))) => Continue(()),
            Expr_::Cast(deref!((Hint(p, _), _))) => {
                env.emit_error(NamingError::ObjectCast(p.clone()));
                Continue(())
            }
            _ => Continue(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use nast::Expr;
    use nast::Pos;

    use super::*;

    #[test]
    fn test_invalid_cast() {
        let mut expr: Expr = elab_utils::expr::from_expr_(Expr_::Cast(Box::new((
            Hint(Pos::NONE, Box::new(Hint_::Hthis)),
            elab_utils::expr::null(),
        ))));
        let env = Env::default();
        expr.transform(&env, &mut ValidateExprCastPass);
        assert_eq!(env.into_errors().len(), 1);
    }
}
