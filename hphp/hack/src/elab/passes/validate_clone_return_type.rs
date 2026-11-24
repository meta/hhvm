// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Hint;
use nast::Hint_;
use nast::Method_;
use nast::Tprim;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ValidateCloneReturnType;

impl Pass for ValidateCloneReturnType {
    fn on_ty_method__bottom_up(&mut self, env: &Env, m: &mut Method_) -> ControlFlow<()> {
        if m.name.name() == sn::members::__CLONE {
            let ret = &m.ret;
            match &ret.1 {
                Some(Hint(_, deref!(Hint_::Hprim(Tprim::Tvoid)))) => {}
                Some(Hint(pos, _)) => env.emit_error(NastCheckError::CloneReturnType(pos.clone())),
                None => {}
            }
        }
        Continue(())
    }
}
