// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use naming_special_names_rust::pseudo_functions;
use nast::CallExpr;
use nast::Expr;
use nast::Expr_;
use nast::Id;
use oxidized::ast;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ElabExprPackagePass;

impl Pass for ElabExprPackagePass {
    fn on_ty_expr_top_down(&mut self, _: &Env, elem: &mut Expr) -> ControlFlow<()> {
        let Expr(_, pos, expr) = elem;
        match expr {
            Expr_::Package(deref!(pkg)) => {
                *expr = Expr_::Call(Box::new(CallExpr {
                    func: Expr::new(
                        (),
                        pos.clone(),
                        Expr_::mk_id(Id(pos.clone(), pseudo_functions::PACKAGE_EXISTS.into())),
                    ),
                    targs: vec![],
                    args: vec![ast::Argument::Anormal(Expr::new(
                        (),
                        pkg.pos().clone(),
                        Expr_::mk_string(pkg.name().into()),
                    ))],
                    unpacked_arg: None,
                }));
                Break(())
            }
            _ => Continue(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use nast::Id;
    use nast::Pos;

    use super::*;

    #[test]
    fn test() {
        let env = Env::default();

        let mut pass = ElabExprPackagePass;
        let mut elem: Expr = Expr(
            (),
            Pos::NONE,
            Expr_::Package(Box::new(Id(Pos::NONE, "foo".into()))),
        );

        elem.transform(&env, &mut pass);

        let Expr(_, _, expr) = elem;
        assert!(match expr {
            Expr_::Call(
                deref!(CallExpr {
                    func: Expr(_, _, Expr_::Id(deref!(Id(_, fn_name)))),
                    args,
                    ..
                }),
            ) if fn_name == pseudo_functions::PACKAGE_EXISTS => {
                if let [ast::Argument::Anormal(Expr(_, _, Expr_::String(fn_param_name)))] =
                    &args[..]
                {
                    fn_param_name == "foo"
                } else {
                    false
                }
            }
            _ => false,
        });
    }
}
