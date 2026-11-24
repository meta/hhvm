// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Class_;
use nast::ClassId;
use nast::ClassId_;
use nast::Expr_;
use nast::Hint;
use nast::Hint_;
use nast::Id;
use oxidized::ast::FunctionPtrId;

use crate::prelude::*;

#[derive(Clone, Default)]
pub struct ValidateExprFunctionPointerPass {
    in_final_class: bool,
    class_name: Option<Rc<String>>,
    parent_name: Option<Rc<String>>,
    is_trait: bool,
}

impl Pass for ValidateExprFunctionPointerPass {
    fn on_ty_class__top_down(&mut self, _env: &Env, class_: &mut Class_) -> ControlFlow<()> {
        self.in_final_class = class_.final_;
        self.is_trait = class_.kind.is_ctrait();
        self.class_name = Some(Rc::new(class_.name.name().to_string()));
        self.parent_name = match class_.extends.as_slice() {
            [Hint(_, deref!(Hint_::Happly(id, _))), ..] => Some(Rc::new(id.name().to_string())),
            _ => None,
        };
        ControlFlow::Continue(())
    }

    fn on_ty_expr__bottom_up(&mut self, env: &Env, expr: &mut Expr_) -> ControlFlow<()> {
        match &expr {
            Expr_::FunctionPointer(deref!((FunctionPtrId::FPId(Id(pos, name)), _))) => {
                // If this comes up more often, we may consider partitioning pseudofunctions
                // into hhi-defined and non-hhi-defined, then just error on all of the hhi-defined ones.
                if name == naming_special_names_rust::pseudo_functions::UNSAFE_CAST {
                    env.emit_error(NamingError::InvalidFunPointer {
                        pos: pos.clone(),
                        name: name.to_string(),
                    });
                }
            }
            Expr_::FunctionPointer(
                deref!((
                    FunctionPtrId::FPClassConst(ClassId(_, pos, class_id_), (_, meth_name)),
                    _,
                )),
            ) => match class_id_ {
                ClassId_::CIself if !self.in_final_class => {
                    env.emit_error(NamingError::SelfInNonFinalFunctionPointer {
                        pos: pos.clone(),
                        meth_name: meth_name.clone(),
                        class_name: if self.is_trait {
                            None
                        } else {
                            self.class_name.as_deref().cloned()
                        },
                    })
                }
                ClassId_::CIparent => env.emit_error(NamingError::ParentInFunctionPointer {
                    pos: pos.clone(),
                    meth_name: meth_name.clone(),
                    parent_name: self.parent_name.as_deref().cloned(),
                }),
                _ => (),
            },
            _ => (),
        }
        ControlFlow::Continue(())
    }
}
