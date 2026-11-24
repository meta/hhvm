// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use oxidized::nast::Expr_;
use oxidized::nast::XhpAttribute;
use oxidized::nast::XhpSimple;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ValidateXhpAttributePass;

impl Pass for ValidateXhpAttributePass {
    fn on_ty_expr__bottom_up(&mut self, env: &Env, expr: &mut Expr_) -> ControlFlow<()> {
        if let Expr_::Xml(deref!((_, attrs, _))) = expr {
            error_if_repeated_attribute(env, attrs);
        }
        Continue(())
    }
}

// Produce a syntax error on XHP expressions of the form: `<foo x={1} x={2} />`
//
// This is not currently enforced in the parser because syntax errors cannot be
// HH_FIXME'd. Once codebases are clean, we can move this check to the parser
// itself.
fn error_if_repeated_attribute(env: &Env, attrs: &[XhpAttribute]) {
    let mut seen = hash::HashSet::<&str>::default();
    for attr in attrs {
        match attr {
            XhpAttribute::XhpSimple(XhpSimple {
                name: (pos, name), ..
            }) if seen.contains(name as &str) => {
                env.emit_error(ParsingError::XhpParsingError {
                    pos: pos.clone(),
                    msg: format!("Cannot redeclare {}", name),
                });
                break; // This isn't usual but consistent with today's OCaml.
            }
            XhpAttribute::XhpSimple(XhpSimple {
                name: (_, name), ..
            }) => {
                seen.insert(name);
            }
            XhpAttribute::XhpSpread(_) => (),
        }
    }
}
