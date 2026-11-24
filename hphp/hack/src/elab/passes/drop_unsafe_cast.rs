// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::CallExpr;
use nast::Expr;
use nast::Expr_;
use nast::Id;

use crate::prelude::*;

#[derive(Clone, Copy)]
pub struct DropUnsafeCast {
    pub emit_checked_unsafe_cast: bool,
}

impl Pass for DropUnsafeCast {
    fn on_ty_expr_bottom_up(&mut self, _env: &Env, elem: &mut Expr) -> ControlFlow<()> {
        let Expr(_, _, expr_) = elem;
        // Checked UNSAFE_NONNULL_CAST is not yet implemented, so it is
        // unconditionally removed
        match expr_ {
            Expr_::Call(
                deref!(CallExpr {
                    func: Expr(_, _, Expr_::Id(deref!(Id(_, fn_name)))),
                    args,
                    ..
                }),
            ) if !args.is_empty()
                && (fn_name == sn::pseudo_functions::UNSAFE_NONNULL_CAST
                    || (!self.emit_checked_unsafe_cast
                        && fn_name == sn::pseudo_functions::UNSAFE_CAST)) =>
            {
                if let nast::Argument::Anormal(e) = args.swap_remove(0) {
                    *elem = e;
                }
            }
            _ => (),
        }
        Continue(())
    }
}
