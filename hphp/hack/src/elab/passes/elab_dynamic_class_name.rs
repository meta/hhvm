// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::ClassGetExpr;
use nast::ClassId;
use nast::ClassId_;
use nast::Expr;
use nast::Expr_;
use nast::FunctionPtrId;
use nast::Id;
use nast::PropOrMethod;

use crate::prelude::*;

#[derive(Copy, Clone, Default)]
pub struct ElabDynamicClassNamePass;

impl Pass for ElabDynamicClassNamePass {
    fn on_ty_expr_bottom_up(&mut self, env: &Env, elem: &mut Expr) -> ControlFlow<()> {
        let invalid = |expr_: &mut Expr_| {
            let inner_expr_ = std::mem::replace(expr_, Expr_::Null);
            let inner_expr = elab_utils::expr::from_expr__with_pos_(elem.1.clone(), inner_expr_);
            *expr_ = Expr_::Invalid(Box::new(Some(inner_expr)));
            Break(())
        };

        match &mut elem.2 {
            Expr_::New(deref!((class_id, _, _, _, _))) if is_dynamic(class_id) => {
                env.emit_error(NamingError::DynamicNewInStrictMode(class_id.1.clone()));
                class_id.2 = ClassId_::CI(Id(class_id.1.clone(), sn::classes::UNKNOWN.to_string()));
                Continue(())
            }
            Expr_::ClassGet(deref!((class_id, ClassGetExpr::CGstring(..), _)))
                if !is_dynamic(class_id) =>
            {
                Continue(())
            }
            Expr_::ClassGet(
                deref!((
                    class_id,
                    ClassGetExpr::CGexpr(cg_expr),
                    PropOrMethod::IsMethod,
                )),
            ) if !is_dynamic(class_id) => {
                env.emit_error(NamingError::DynamicMethodAccess(cg_expr.1.clone()));
                Continue(())
            }
            Expr_::ClassGet(
                deref!((
                    ClassId(_, _, ClassId_::CIexpr(ci_expr)),
                    ClassGetExpr::CGstring(..)
                        | ClassGetExpr::CGexpr(Expr(_, _, Expr_::Lvar(..) | Expr_::This)),
                    _,
                )),
            ) => {
                env.emit_error(NamingError::DynamicClassNameInStrictMode(ci_expr.1.clone()));
                invalid(&mut elem.2)
            }
            Expr_::ClassGet(
                deref!((
                    ClassId(_, _, ClassId_::CIexpr(ci_expr)),
                    ClassGetExpr::CGexpr(cg_expr),
                    _,
                )),
            ) => {
                env.emit_error(NamingError::DynamicClassNameInStrictMode(ci_expr.1.clone()));
                env.emit_error(NamingError::DynamicClassNameInStrictMode(cg_expr.1.clone()));
                invalid(&mut elem.2)
            }
            Expr_::ClassGet(deref!((_, ClassGetExpr::CGexpr(cg_expr), _))) => {
                env.emit_error(NamingError::DynamicClassNameInStrictMode(cg_expr.1.clone()));
                invalid(&mut elem.2)
            }
            Expr_::FunctionPointer(deref!((FunctionPtrId::FPClassConst(class_id, _), _)))
                if is_dynamic(class_id) =>
            {
                invalid(&mut elem.2)
            }
            Expr_::ClassConst(deref!((class_id, _))) if is_dynamic(class_id) => {
                invalid(&mut elem.2)
            }
            _ => Continue(()),
        }
    }
}

fn is_dynamic(class_id: &ClassId) -> bool {
    match &class_id.2 {
        ClassId_::CIparent
        | ClassId_::CIself
        | ClassId_::CIstatic
        | ClassId_::CI(..)
        | ClassId_::CIreified(..)
        | ClassId_::CIexpr(Expr(
            _,
            _,
            Expr_::Lvar(..)
            | Expr_::This
            | Expr_::Dollardollar(..)
            | Expr_::Await(..)
            | Expr_::Delay(..),
        )) => false,
        ClassId_::CIexpr(_) => true,
    }
}

#[cfg(test)]
mod tests {

    use nast::Pos;

    use super::*;

    fn mk_dynamic_class_id() -> ClassId {
        ClassId(
            (),
            Pos::default(),
            ClassId_::CIexpr(elab_utils::expr::null()),
        )
    }

    fn mk_non_dynamic_class_id() -> ClassId {
        ClassId((), Pos::default(), ClassId_::CIself)
    }

    // -- in `New` expressions -------------------------------------------------

    #[test]
    fn test_new_valid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::New(Box::new((
                mk_non_dynamic_class_id(),
                Default::default(),
                Default::default(),
                Default::default(),
                Default::default(),
            ))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        let Expr(_, _, expr_) = elem;
        assert!(matches!(
            expr_,
            Expr_::New(deref!((ClassId(_, _, ClassId_::CIself), _, _, _, _)))
        ));
    }

    #[test]
    fn test_new_invalid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::New(Box::new((
                mk_dynamic_class_id(),
                Default::default(),
                Default::default(),
                Default::default(),
                Default::default(),
            ))),
        );
        elem.transform(&env, &mut pass);

        let err_opt = env.into_errors().pop();
        assert!(matches!(
            err_opt,
            Some(NamingPhaseError::Naming(
                NamingError::DynamicNewInStrictMode(..)
            ))
        ));
        let Expr(_, _, expr_) = elem;
        assert!(matches!(
            expr_,
            Expr_::New(deref!((ClassId(_, _, ClassId_::CI(..)), _, _, _, _)))
        ))
    }

    // -- in `FunctionPointer` expressions -------------------------------------
    #[test]
    fn test_function_pointer_valid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::FunctionPointer(Box::new((
                FunctionPtrId::FPClassConst(mk_non_dynamic_class_id(), Default::default()),
                Default::default(),
            ))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        let Expr(_, _, expr_) = elem;
        assert!(matches!(
            expr_,
            Expr_::FunctionPointer(deref!((
                FunctionPtrId::FPClassConst(ClassId(_, _, ClassId_::CIself), _),
                _,
            )))
        ));
    }

    #[test]
    fn test_function_pointer_invalid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::FunctionPointer(Box::new((
                FunctionPtrId::FPClassConst(mk_dynamic_class_id(), Default::default()),
                Default::default(),
            ))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        let Expr(_, _, expr_) = elem;
        assert!(matches!(expr_, Expr_::Invalid(_)))
    }

    // -- in `ClassConst` expressions ------------------------------------------

    #[test]
    fn test_class_const_valid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::ClassConst(Box::new((mk_non_dynamic_class_id(), Default::default()))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(
            elem,
            Expr(
                _,
                _,
                Expr_::ClassConst(deref!((ClassId(_, _, ClassId_::CIself), _)))
            )
        ))
    }

    #[test]
    fn test_class_const_invalid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::ClassConst(Box::new((mk_dynamic_class_id(), Default::default()))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(
            elem,
            Expr(
                _,
                _,
                Expr_::Invalid(deref!(Some(Expr(
                    _,
                    _,
                    Expr_::ClassConst(deref!((ClassId(_, _, ClassId_::CIexpr(..)), _))),
                )))),
            )
        ))
    }

    // -- in `ClassGet` expressions --------------------------------------------
    #[test]
    fn test_class_get_cg_string_valid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::ClassGet(Box::new((
                mk_non_dynamic_class_id(),
                ClassGetExpr::CGstring(Default::default()),
                PropOrMethod::IsProp,
            ))),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(
            elem,
            Expr(
                _,
                _,
                Expr_::ClassGet(deref!((ClassId(_, _, ClassId_::CIself), _, _)))
            )
        ))
    }

    #[test]
    fn test_class_get_cg_string_invalid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::ClassGet(Box::new((
                mk_dynamic_class_id(),
                ClassGetExpr::CGstring(Default::default()),
                PropOrMethod::IsProp,
            ))),
        );
        elem.transform(&env, &mut pass);

        let err_opt = env.into_errors().pop();
        assert!(matches!(
            err_opt,
            Some(NamingPhaseError::Naming(
                NamingError::DynamicClassNameInStrictMode(..)
            ))
        ));
        assert!(matches!(elem, Expr(_, _, Expr_::Invalid(_))))
    }

    #[test]
    fn test_class_get_cg_expr_invalid() {
        let env = Env::default();

        let mut pass = ElabDynamicClassNamePass;
        let mut elem = Expr(
            (),
            Pos::default(),
            Expr_::ClassGet(Box::new((
                mk_dynamic_class_id(),
                ClassGetExpr::CGexpr(elab_utils::expr::null()),
                PropOrMethod::IsProp,
            ))),
        );
        elem.transform(&env, &mut pass);

        assert!(matches!(
            env.into_errors().as_slice(),
            [
                NamingPhaseError::Naming(NamingError::DynamicClassNameInStrictMode(..)),
                NamingPhaseError::Naming(NamingError::DynamicClassNameInStrictMode(..))
            ]
        ));
        assert!(matches!(elem, Expr(_, _, Expr_::Invalid(_))))
    }
}
