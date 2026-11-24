// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Expr_;
use nast::Hint;
use nast::Hint_;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ValidateDynamicHintPass;

impl Pass for ValidateDynamicHintPass {
    fn on_ty_expr__bottom_up(&mut self, env: &Env, expr: &mut Expr_) -> ControlFlow<()> {
        match &*expr {
            Expr_::Is(deref!((_, Hint(p, deref!(Hint_::Hdynamic))))) => {
                env.emit_error(NamingError::DynamicHintDisallowed(p.clone()));
                Continue(())
            }
            _ => Continue(()),
        }
    }
}
