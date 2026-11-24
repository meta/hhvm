// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Class_;
use nast::ClassId;
use nast::ClassId_;
use nast::ClassishKind;
use nast::Expr;
use nast::Expr_;
use nast::FunDef;
use nast::Id;
use nast::Method_;

use crate::prelude::*;

#[derive(Clone, Default)]
pub struct ValidateIllegalNamePass {
    func_name: Option<Rc<String>>,
    classish_kind: Option<ClassishKind>,
}

impl ValidateIllegalNamePass {
    fn is_current_func(&self, nm: &str) -> bool {
        if let Some(cur_nm) = self.func_name.as_deref() {
            return cur_nm == nm;
        }
        false
    }

    fn in_trait(&self) -> bool {
        self.classish_kind
            .map_or(false, |ck| ck == ClassishKind::Ctrait)
    }
}

impl Pass for ValidateIllegalNamePass {
    fn on_ty_class__top_down(&mut self, _: &Env, elem: &mut Class_) -> ControlFlow<()> {
        self.classish_kind = Some(elem.kind);
        Continue(())
    }

    fn on_ty_class__bottom_up(&mut self, env: &Env, elem: &mut Class_) -> ControlFlow<()> {
        elem.typeconsts
            .iter()
            .for_each(|tc| check_illegal_member_variable_class(env, &tc.name));
        elem.consts
            .iter()
            .for_each(|cc| check_illegal_member_variable_class(env, &cc.id));
        Continue(())
    }

    fn on_ty_fun_def_top_down(&mut self, _: &Env, elem: &mut FunDef) -> ControlFlow<()> {
        self.func_name = Some(Rc::new(elem.name.name().to_string()));
        Continue(())
    }

    fn on_ty_fun_def_bottom_up(&mut self, env: &Env, elem: &mut FunDef) -> ControlFlow<()> {
        let name = elem.name.name();
        let lower_norm = name
            .strip_prefix('\\')
            .unwrap_or(name)
            .to_ascii_lowercase()
            .to_string();
        if lower_norm == sn::members::__CONSTRUCT || lower_norm == "using" {
            env.emit_error(NastCheckError::IllegalFunctionName {
                pos: elem.name.pos().clone(),
                name: elem.name.name().to_string(),
            })
        }
        Continue(())
    }

    fn on_ty_method__top_down(&mut self, _: &Env, elem: &mut Method_) -> ControlFlow<()> {
        self.func_name = Some(Rc::new(elem.name.name().to_string()));
        Continue(())
    }

    fn on_ty_method__bottom_up(&mut self, env: &Env, elem: &mut Method_) -> ControlFlow<()> {
        if elem.name.name() == sn::members::__DESTRUCT {
            env.emit_error(NastCheckError::IllegalDestructor(elem.name.pos().clone()))
        }
        Continue(())
    }

    fn on_ty_expr__bottom_up(&mut self, env: &Env, elem: &mut Expr_) -> ControlFlow<()> {
        match elem {
            Expr_::Id(deref!(id))
                if id.name() == sn::pseudo_consts::G__CLASS__ && self.classish_kind.is_none() =>
            {
                env.emit_error(NamingError::IllegalCLASS(id.pos().clone()))
            }

            Expr_::Id(deref!(id))
                if id.name() == sn::pseudo_consts::G__TRAIT__ && !self.in_trait() =>
            {
                env.emit_error(NamingError::IllegalTRAIT(id.pos().clone()))
            }

            Expr_::ClassConst(deref!((ClassId(_, _, ClassId_::CIparent), (_, meth_name))))
                if self.is_current_func(meth_name) =>
            {
                ()
            }

            Expr_::ClassConst(deref!((_, (pos, meth_name))))
                if is_magic(meth_name) && !self.is_current_func(meth_name) =>
            {
                env.emit_error(NastCheckError::Magic {
                    pos: pos.clone(),
                    meth_name: meth_name.clone(),
                })
            }

            Expr_::ObjGet(deref!((_, Expr(_, _, Expr_::Id(deref!(id))), _, _)))
                if is_magic(id.name()) =>
            {
                env.emit_error(NastCheckError::Magic {
                    pos: id.pos().clone(),
                    meth_name: id.name().to_string(),
                })
            }

            Expr_::MethodCaller(deref!((_, (pos, meth_name)))) if is_magic(meth_name) => env
                .emit_error(NastCheckError::Magic {
                    pos: pos.clone(),
                    meth_name: meth_name.clone(),
                }),
            _ => (),
        }
        Continue(())
    }
}

fn check_illegal_member_variable_class(env: &Env, id: &Id) {
    if id.name().to_ascii_lowercase() == sn::members::M_CLASS {
        env.emit_error(NamingError::IllegalMemberVariableClass(id.pos().clone()))
    }
}

fn is_magic(nm: &str) -> bool {
    nm != sn::members::__TO_STRING && sn::members::AS_SET.contains(nm)
}
