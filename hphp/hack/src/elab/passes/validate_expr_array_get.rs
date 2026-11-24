// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Binop;
use nast::Expr;
use nast::Expr_;
use oxidized::ast_defs::Bop;

use crate::prelude::*;

#[derive(Copy, Clone, Default)]
pub struct ValidateExprArrayGetPass {
    array_append_allowed: bool,
    is_assignment: bool,
}

impl Pass for ValidateExprArrayGetPass {
    fn on_ty_expr_bottom_up(&mut self, env: &Env, expr: &mut Expr) -> ControlFlow<()> {
        match &expr.2 {
            Expr_::ArrayGet(deref!((array_get_expr, None))) if !self.array_append_allowed => {
                env.emit_error(NastCheckError::ReadingFromAppend(array_get_expr.1.clone()))
            }
            _ => (),
        }
        Continue(())
    }

    fn on_ty_expr__top_down(&mut self, _env: &Env, expr_: &mut Expr_) -> ControlFlow<()> {
        match expr_ {
            Expr_::ObjGet(..) | Expr_::ArrayGet(..) => self.array_append_allowed = false,
            _ => (),
        }
        Continue(())
    }

    fn on_ty_binop_top_down(&mut self, _env: &Env, binop: &mut Binop) -> ControlFlow<()> {
        if binop.bop == Bop::Eq(None) {
            self.is_assignment = true;
        }
        Continue(())
    }

    fn on_fld_binop_lhs_top_down(&mut self, _env: &Env, _elem: &mut Expr) -> ControlFlow<()> {
        self.array_append_allowed = self.is_assignment;
        Continue(())
    }
}
