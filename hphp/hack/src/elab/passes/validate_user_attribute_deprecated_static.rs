// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::ops::ControlFlow;

use nast::Binop;
use nast::Expr;
use nast::Expr_;
use nast::Fun_;
use nast::Method_;
use nast::Pos;
use nast::UserAttribute;
use oxidized::ast_defs::Bop;

use crate::prelude::*;

#[derive(Copy, Clone, Default)]
pub struct ValidateUserAttributeDeprecatedStaticPass;

impl Pass for ValidateUserAttributeDeprecatedStaticPass {
    fn on_ty_fun__bottom_up(&mut self, env: &Env, elem: &mut Fun_) -> ControlFlow<(), ()> {
        check_deprecated_static(&elem.user_attributes, env);
        ControlFlow::Continue(())
    }

    fn on_ty_method__bottom_up(&mut self, env: &Env, elem: &mut Method_) -> ControlFlow<(), ()> {
        check_deprecated_static(&elem.user_attributes, env);
        ControlFlow::Continue(())
    }
}

fn check_deprecated_static(user_attrs: &[UserAttribute], env: &Env) {
    user_attrs
        .iter()
        .find(|ua| ua.name.name() == sn::user_attributes::DEPRECATED)
        .iter()
        .for_each(|ua| match ua.params.as_slice() {
            [msg, ..] => {
                if let Some(pos) = is_static_string(msg) {
                    env.emit_error(NastCheckError::AttributeParamType {
                        pos,
                        x: "static string literal".to_string(),
                    })
                }
            }
            _ => (),
        })
}

fn is_static_string(expr: &Expr) -> Option<Pos> {
    let mut exprs = vec![expr];
    while let Some(expr) = exprs.pop() {
        match &expr.2 {
            Expr_::Binop(
                deref!(Binop {
                    bop: Bop::Dot,
                    lhs,
                    rhs,
                }),
            ) => {
                exprs.push(lhs);
                exprs.push(rhs);
            }
            Expr_::String(..) => (),
            _ => return Some(expr.1.clone()),
        }
    }
    None
}
