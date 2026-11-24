// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use bitflags::bitflags;
use nast::Binop;
use nast::Bop;
use nast::CallExpr;
use nast::Class_;
use nast::ClassConstKind;
use nast::ClassId;
use nast::ClassId_;
use nast::ClassishKind;
use nast::Expr;
use nast::Expr_;
use nast::FunDef;
use nast::Gconst;
use nast::Hint;
use nast::Hint_;
use nast::KvcKind;
use nast::ModuleDef;
use nast::Typedef;
use nast::Uop;
use nast::VcKind;

use crate::prelude::*;

#[derive(Copy, Clone)]
pub struct ElabConstExprPass {
    mode: file_info::Mode,
    flags: Flags,
}

bitflags! {
    #[derive(Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Clone, Copy)]
    struct Flags: u8 {
        const IN_ENUM_CLASS = 1 << 0;
        const ENFORCE_CONST_EXPR = 1 << 1;
    }
}

impl ElabConstExprPass {
    pub fn in_enum_class(&self) -> bool {
        self.flags.contains(Flags::IN_ENUM_CLASS)
    }

    pub fn set_in_enum_class(&mut self, value: bool) {
        self.flags.set(Flags::IN_ENUM_CLASS, value)
    }

    pub fn enforce_const_expr(&self) -> bool {
        self.flags.contains(Flags::ENFORCE_CONST_EXPR)
    }

    pub fn set_enforce_const_expr(&mut self, value: bool) {
        self.flags.set(Flags::ENFORCE_CONST_EXPR, value)
    }
}

impl Default for ElabConstExprPass {
    fn default() -> Self {
        ElabConstExprPass {
            mode: file_info::Mode::Mstrict,
            flags: Flags::default(),
        }
    }
}

impl Pass for ElabConstExprPass {
    // We can determine that certain expressions are invalid based on one-level
    // pattern matching. We prefer to do this since we can stop the transformation
    // early in these cases. For cases where we need to pattern match on the
    // expression more deeply, we use the bottom-up pass
    fn on_ty_expr_top_down(&mut self, env: &Env, elem: &mut Expr) -> ControlFlow<()> {
        if !self.enforce_const_expr() {
            Continue(())
        } else {
            let Expr(_, pos, expr_) = elem;
            let invalid = |expr_: &mut Expr_| {
                let inner_expr_ = std::mem::replace(expr_, Expr_::Null);
                let inner_expr = Expr(Default::default(), pos.clone(), inner_expr_);
                *expr_ = Expr_::Invalid(Box::new(Some(inner_expr)));
                Break(())
            };

            match &expr_ {
                // -- always valid ---------------------------------------------
                Expr_::Id(..)
                | Expr_::Null
                | Expr_::True
                | Expr_::False
                | Expr_::Int(_)
                | Expr_::Float(_)
                | Expr_::String(_)
                | Expr_::FunctionPointer(..)
                | Expr_::Eif(..)
                | Expr_::Tuple(..)
                | Expr_::Shape(..)
                | Expr_::Upcast(..) => Continue(()),

                // -- markers --------------------------------------------------
                Expr_::Invalid(..) | Expr_::Hole(..) => Continue(()),

                // -- handled bottom-up ----------------------------------------
                Expr_::Nameof(_) => Continue(()),
                Expr_::ClassConst(_) => Continue(()),

                // -- conditionally valid --------------------------------------
                Expr_::As(
                    deref!(nast::As_ {
                        hint: Hint(_, deref!(hint_)),
                        ..
                    }),
                ) => match hint_ {
                    // NB we can perform this top-down since the all valid hints
                    // are already in canonical
                    // TODO[mjt] another example of inconsistency around error positions
                    Hint_::Happly(id, _) => {
                        env.emit_error(NamingError::IllegalConstant(id.0.clone()));
                        invalid(expr_)
                    }
                    _ => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                },
                Expr_::Unop(deref!((uop, _))) => match uop {
                    Uop::Uplus | Uop::Uminus | Uop::Utild | Uop::Unot => Continue(()),
                    _ => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                },
                Expr_::Binop(deref!(Binop { bop, .. })) => match bop {
                    Bop::Eq(_) => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                    _ => Continue(()),
                },
                Expr_::ValCollection(deref!(((_, vc_kind), _, _))) => match vc_kind {
                    VcKind::Vec | VcKind::Keyset => Continue(()),
                    _ => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                },
                Expr_::KeyValCollection(deref!(((_, kvc_kind), _, _))) => match kvc_kind {
                    KvcKind::Dict => Continue(()),
                    _ => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                },
                Expr_::Call(
                    deref!(CallExpr {
                        func: Expr(_, _, call_expr_),
                        ..
                    }),
                ) => match call_expr_ {
                    Expr_::Id(deref!(id))
                        if id.name() == sn::std_lib_functions::ARRAY_MARK_LEGACY
                            || id.name() == sn::pseudo_functions::UNSAFE_CAST
                            || id.name() == sn::pseudo_functions::UNSAFE_NONNULL_CAST =>
                    {
                        Continue(())
                    }
                    _ => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                },

                Expr_::Omitted => {
                    if matches!(self.mode, file_info::Mode::Mhhi) {
                        Continue(())
                    } else {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }
                }

                // -- always invalid -------------------------------------------
                Expr_::This
                | Expr_::Lvar(..)
                | Expr_::Lplaceholder(..)
                | Expr_::ArrayGet(..)
                | Expr_::Await(..)
                | Expr_::Delay(..)
                | Expr_::Cast(..)
                | Expr_::ClassGet(..)
                | Expr_::Clone(..)
                | Expr_::Dollardollar(..)
                | Expr_::ETSplice(..)
                | Expr_::Efun(..)
                | Expr_::EnumClassLabel(..)
                | Expr_::ExpressionTree(..)
                | Expr_::Is(..)
                | Expr_::Lfun(..)
                | Expr_::List(..)
                | Expr_::MethodCaller(..)
                | Expr_::New(..)
                | Expr_::ObjGet(..)
                | Expr_::Pair(..)
                | Expr_::Pipe(..)
                | Expr_::PrefixedString(..)
                | Expr_::ReadonlyExpr(..)
                | Expr_::String2(..)
                | Expr_::Yield(..)
                | Expr_::Xml(..)
                | Expr_::Package(..) => {
                    env.emit_error(NamingError::IllegalConstant(pos.clone()));
                    invalid(expr_)
                }

                // -- unexpected expressions -----------------------------------
                Expr_::Import(..) | Expr_::Collection(..) => panic!("Unexpected Expr"),
            }
        }
    }

    // Handle non-constant expressions which require pattern matching on some
    // element of the expression which is not yet transformed in the top-down pass
    fn on_ty_expr_bottom_up(&mut self, env: &Env, elem: &mut Expr) -> ControlFlow<()> {
        if !self.enforce_const_expr() {
            Continue(())
        } else {
            let Expr(_, pos, expr_) = elem;
            let invalid = |expr_: &mut Expr_| {
                let inner_expr_ = std::mem::replace(expr_, Expr_::Null);
                let inner_expr = Expr(Default::default(), pos.clone(), inner_expr_);
                *expr_ = Expr_::Invalid(Box::new(Some(inner_expr)));
                Break(())
            };

            match expr_ {
                Expr_::Nameof(deref!(ClassId(_, _, class_id_)))
                | Expr_::ClassConst(deref!((ClassId(_, _, class_id_), _))) => match class_id_ {
                    ClassId_::CIstatic => {
                        env.emit_error(NamingError::IllegalConstant(pos.clone()));
                        invalid(expr_)
                    }

                    ClassId_::CIparent | ClassId_::CIself | ClassId_::CI(..) => Continue(()),
                    ClassId_::CIreified(..) => {
                        // TODO(T259578698) this is a bug (from the CIexpr(Id) case), allows class-bound reified in constexpr
                        Continue(())
                    }
                    ClassId_::CIexpr(Expr(_, _, expr_)) => match expr_ {
                        Expr_::This | Expr_::Id(..) => Continue(()),
                        _ => {
                            env.emit_error(NamingError::IllegalConstant(pos.clone()));
                            invalid(expr_)
                        }
                    },
                },
                _ => Continue(()),
            }
        }
    }

    fn on_ty_class__top_down(&mut self, _: &Env, elem: &mut Class_) -> ControlFlow<()> {
        self.set_in_enum_class(match elem.kind {
            ClassishKind::CenumClass(_) => true,
            ClassishKind::Cclass(..)
            | ClassishKind::Cinterface
            | ClassishKind::Cenum
            | ClassishKind::Ctrait => false,
        });
        self.mode = elem.mode;
        Continue(())
    }

    fn on_ty_class_const_kind_top_down(
        &mut self,
        _: &Env,
        elem: &mut ClassConstKind,
    ) -> ControlFlow<()> {
        self.set_enforce_const_expr(
            !self.in_enum_class() && matches!(elem, ClassConstKind::CCConcrete(_)),
        );
        Continue(())
    }

    fn on_ty_typedef_top_down(&mut self, _: &Env, elem: &mut Typedef) -> ControlFlow<()> {
        self.mode = elem.mode;
        Continue(())
    }

    fn on_ty_gconst_top_down(&mut self, _: &Env, elem: &mut Gconst) -> ControlFlow<()> {
        self.mode = elem.mode;
        self.set_enforce_const_expr(true);
        Continue(())
    }

    fn on_ty_fun_def_top_down(&mut self, _: &Env, elem: &mut FunDef) -> ControlFlow<()> {
        self.mode = elem.mode;
        Continue(())
    }

    fn on_ty_module_def_top_down(&mut self, _: &Env, elem: &mut ModuleDef) -> ControlFlow<()> {
        self.mode = elem.mode;
        Continue(())
    }
}
