// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use bitflags::bitflags;
use nast::Class_;
use nast::Contexts;
use nast::Hint;
use nast::Hint_;
use nast::Id;
use nast::WhereConstraintHint;

use crate::prelude::*;

#[derive(Clone, Default)]
pub struct ElabHintHaccessPass {
    current_class: Option<Rc<Id>>,
    flags: Flags,
}

bitflags! {
    #[derive(Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Clone, Copy)]
    struct Flags: u8 {
        const IN_CONTEXT = 1 << 0;
        const IN_HACCESS = 1 << 1;
        const IN_WHERE_CLAUSE = 1 << 2;
    }
}

impl ElabHintHaccessPass {
    fn set_in_class(&mut self, cls: &Class_) {
        self.current_class = Some(Rc::new(cls.name.clone()))
    }

    fn in_context(&self) -> bool {
        self.flags.contains(Flags::IN_CONTEXT)
    }

    fn set_in_context(&mut self, value: bool) {
        self.flags.set(Flags::IN_CONTEXT, value)
    }

    fn in_haccess(&self) -> bool {
        self.flags.contains(Flags::IN_HACCESS)
    }

    fn set_in_haccess(&mut self, value: bool) {
        self.flags.set(Flags::IN_HACCESS, value)
    }

    fn in_where_clause(&self) -> bool {
        self.flags.contains(Flags::IN_WHERE_CLAUSE)
    }

    fn set_in_where_clause(&mut self, value: bool) {
        self.flags.set(Flags::IN_WHERE_CLAUSE, value)
    }
}

impl Pass for ElabHintHaccessPass {
    fn on_ty_hint_top_down(&mut self, env: &Env, elem: &mut Hint) -> ControlFlow<()> {
        if !self.in_haccess() {
            return Continue(());
        }
        match &mut *elem.1 {
            Hint_::Happly(id, hints) if id.name() == sn::classes::SELF => {
                if let Some(class_id) = self.current_class.as_deref() {
                    *id = class_id.clone();
                    // TODO[mjt] we appear to be discarding type arguments on `Happly` here?
                    hints.clear();
                    Continue(())
                } else {
                    env.emit_error(NamingError::SelfOutsideClass(id.0.clone()));
                    Break(())
                }
            }

            Hint_::Happly(id, _)
                if id.name() == sn::classes::STATIC || id.name() == sn::classes::PARENT =>
            {
                env.emit_error(NamingError::InvalidTypeAccessRoot {
                    pos: id.pos().clone(),
                    id: Some(id.name().to_string()),
                });
                Break(())
            }

            Hint_::Hthis | Hint_::Happly(..) => Continue(()),

            Hint_::Habstr(..) if self.in_where_clause() || self.in_context() => Continue(()),

            // TODO[mjt] I don't understand what this case corresponds to
            Hint_::Hvar(..) => Continue(()),

            _ => {
                env.emit_error(NamingError::InvalidTypeAccessRoot {
                    pos: elem.0.clone(),
                    id: None,
                });
                Break(())
            }
        }
    }

    fn on_ty_hint__top_down(&mut self, _: &Env, elem: &mut Hint_) -> ControlFlow<()> {
        self.set_in_haccess(matches!(elem, Hint_::Haccess(..)));
        Continue(())
    }

    fn on_ty_class__top_down(&mut self, _: &Env, elem: &mut Class_) -> ControlFlow<()> {
        self.set_in_class(elem);
        Continue(())
    }

    fn on_ty_where_constraint_hint_top_down(
        &mut self,
        _: &Env,
        _: &mut WhereConstraintHint,
    ) -> ControlFlow<()> {
        self.set_in_where_clause(true);
        Continue(())
    }

    fn on_ty_contexts_top_down(&mut self, _: &Env, _: &mut Contexts) -> ControlFlow<()> {
        self.set_in_context(true);
        Continue(())
    }
}

#[cfg(test)]
mod tests {

    use nast::ConstraintKind;
    use nast::Id;
    use nast::Pos;

    use super::*;

    // -- type access through `self` -------------------------------------------

    #[test]
    // type access through `self` is valid inside a class
    fn test_haccess_self_in_class_valid() {
        let env = Env::default();

        let class_name = "Classy";
        let mut pass = ElabHintHaccessPass {
            current_class: Some(Rc::new(Id(Default::default(), "Classy".to_string()))),
            ..Default::default()
        };
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(
                    Pos::default(),
                    Box::new(Hint_::Happly(
                        Id(Pos::default(), sn::classes::SELF.to_string()),
                        vec![],
                    )),
                ),
                vec![Id(Pos::default(), "T".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(match elem {
            Hint(_, deref!(Hint_::Haccess(Hint(_, deref!(Hint_::Happly(id, hints))), _))) =>
                id.name() == class_name && hints.is_empty(),
            _ => false,
        })
    }

    #[test]
    // currently we erase any type params to which `self` is applied; adding
    // test for tracking
    fn test_haccess_self_in_class_erased_tparams_valid() {
        let env = Env::default();

        let class_name = "Classy";
        let mut pass = ElabHintHaccessPass {
            current_class: Some(Rc::new(Id(Default::default(), "Classy".to_string()))),
            ..Default::default()
        };
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(
                    Pos::default(),
                    Box::new(Hint_::Happly(
                        Id(Pos::default(), sn::classes::SELF.to_string()),
                        vec![Hint(Pos::default(), Box::new(Hint_::Hmixed))],
                    )),
                ),
                vec![Id(Pos::default(), "T".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(match elem {
            Hint(_, deref!(Hint_::Haccess(Hint(_, deref!(Hint_::Happly(id, hints))), _))) =>
                id.name() == class_name && hints.is_empty(),
            _ => false,
        })
    }

    #[test]
    // type access through `self` is invalid outside a class
    fn test_haccess_self_outside_class_invalid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass::default();
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(
                    Pos::default(),
                    Box::new(Hint_::Happly(
                        Id(Pos::default(), sn::classes::SELF.to_string()),
                        vec![],
                    )),
                ),
                vec![Id(Pos::default(), "T".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        let err_opt = env.into_errors().pop();
        assert!(matches!(
            err_opt,
            Some(NamingPhaseError::Naming(NamingError::SelfOutsideClass(..)))
        ));
    }
    // -- type access through `this` -------------------------------------------

    #[test]
    // type access through `this` is always valid
    fn test_haccess_this_valid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass::default();
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(Pos::default(), Box::new(Hint_::Hthis)),
                vec![Id(Pos::default(), "T".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        assert!(matches!(
            elem,
            Hint(_, deref!(Hint_::Haccess(Hint(_, deref!(Hint_::Hthis)), _)))
        ))
    }

    // -- type access through `static` / `parent` ------------------------------
    #[test]
    // type access through `static` is invalid
    fn test_haccess_static_in_class_invalid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass {
            current_class: Some(Rc::new(Id(Default::default(), "Classy".to_string()))),
            ..Default::default()
        };
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(
                    Pos::default(),
                    Box::new(Hint_::Happly(
                        Id(Pos::default(), sn::classes::STATIC.to_string()),
                        vec![],
                    )),
                ),
                vec![Id(Pos::default(), "T".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        let err_opt = env.into_errors().pop();
        assert!(matches!(
            err_opt,
            Some(NamingPhaseError::Naming(
                NamingError::InvalidTypeAccessRoot { .. }
            ))
        ));
    }

    // -- type access through type param ---------------------------------------

    #[test]
    // type access through type parameter is valid inside a context
    fn test_haccess_tparam_in_context_valid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass::default();
        let mut elem = Contexts(
            Pos::default(),
            vec![Hint(
                Pos::default(),
                Box::new(Hint_::Haccess(
                    Hint(
                        Pos::default(),
                        Box::new(Hint_::Habstr("T".to_string(), vec![])),
                    ),
                    vec![Id(Pos::default(), "C".to_string())],
                )),
            )],
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        let Contexts(_, mut hints) = elem;
        assert!(matches!(
            hints.pop(),
            Some(Hint(
                _,
                deref!(Hint_::Haccess(Hint(_, deref!(Hint_::Habstr(..))), _))
            ))
        ))
    }

    #[test]
    // type access through type parameter is valid inside a where clause
    fn test_haccess_tparam_in_where_clause_valid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass::default();
        let mut elem = WhereConstraintHint(
            Hint(
                Pos::default(),
                Box::new(Hint_::Haccess(
                    Hint(
                        Pos::default(),
                        Box::new(Hint_::Habstr("T".to_string(), vec![])),
                    ),
                    vec![Id(Pos::default(), "C".to_string())],
                )),
            ),
            ConstraintKind::ConstraintSuper,
            Hint(Pos::default(), Box::new(Hint_::Hmixed)),
        );
        elem.transform(&env, &mut pass);

        assert!(env.into_errors().is_empty());
        let WhereConstraintHint(hint, _, _) = elem;
        assert!(matches!(
            hint,
            Hint(
                _,
                deref!(Hint_::Haccess(Hint(_, deref!(Hint_::Habstr(..))), _))
            )
        ))
    }

    #[test]
    // type access through type parameter in any other context is invalid
    fn test_haccess_tparam_invalid() {
        let env = Env::default();

        let mut pass = ElabHintHaccessPass::default();
        let mut elem = Hint(
            Pos::default(),
            Box::new(Hint_::Haccess(
                Hint(
                    Pos::default(),
                    Box::new(Hint_::Habstr("T".to_string(), vec![])),
                ),
                vec![Id(Pos::default(), "C".to_string())],
            )),
        );
        elem.transform(&env, &mut pass);

        let err_opt = env.into_errors().pop();
        assert!(matches!(
            err_opt,
            Some(NamingPhaseError::Naming(
                NamingError::InvalidTypeAccessRoot { .. }
            ))
        ));
    }
}
