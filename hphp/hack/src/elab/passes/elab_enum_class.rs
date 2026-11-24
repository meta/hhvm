// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use nast::Abstraction;
use nast::Class_;
use nast::ClassishKind;
use nast::Expr_;
use nast::Hint;
use nast::Hint_;
use nast::Id;
use nast::Pos;
use nast::UserAttributes;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ElabEnumClassPass;

impl Pass for ElabEnumClassPass {
    fn on_ty_class__bottom_up(&mut self, _: &Env, elem: &mut Class_) -> ControlFlow<()> {
        if let Some(enum_) = &elem.enum_ {
            let Id(pos, _) = &elem.name;
            let enum_hint = Hint(
                pos.clone(),
                Box::new(Hint_::Happly(elem.name.clone(), vec![])),
            );
            let (cls_name, bounds) = match elem.kind {
                ClassishKind::CenumClass(Abstraction::Concrete) => (
                    sn::classes::HH_BUILTIN_ENUM_CLASS,
                    vec![Hint(
                        pos.clone(),
                        Box::new(Hint_::Happly(
                            Id(pos.clone(), sn::classes::MEMBER_OF.to_string()),
                            vec![enum_hint, enum_.base.clone()],
                        )),
                    )],
                ),
                ClassishKind::CenumClass(Abstraction::Abstract) => {
                    (sn::classes::HH_BUILTIN_ABSTRACT_ENUM_CLASS, vec![])
                }
                _ => (sn::classes::HH_BUILTIN_ENUM, vec![enum_hint]),
            };
            let extend_hint = Hint(
                pos.clone(),
                Box::new(Hint_::Happly(Id(pos.clone(), cls_name.to_string()), bounds)),
            );
            elem.extends.push(extend_hint)
        }
        Continue(())
    }

    fn on_ty_hint__bottom_up(&mut self, env: &Env, elem: &mut Hint_) -> ControlFlow<()> {
        if !(env.is_hhi() || env.is_systemlib()) {
            match elem {
                Hint_::Happly(Id(pos, ty_name), _)
                    if ty_name == sn::classes::HH_BUILTIN_ENUM
                        || ty_name == sn::classes::HH_BUILTIN_ENUM_CLASS
                        || ty_name == sn::classes::HH_BUILTIN_ABSTRACT_ENUM_CLASS =>
                {
                    env.emit_error(NamingError::UsingInternalClass {
                        pos: pos.clone(),
                        class_name: core_utils_rust::strip_ns(ty_name).to_string(),
                    })
                }
                _ => (),
            }
        }
        Continue(())
    }

    fn on_ty_user_attributes_bottom_up(
        &mut self,
        env: &Env,
        elem: &mut UserAttributes,
    ) -> ControlFlow<()> {
        elem.iter_mut().for_each(|ua| {
            let attr_name = ua.name.name();
            if sn::user_attributes::MEMOIZE == attr_name
                || sn::user_attributes::MEMOIZE_LSB == attr_name
            {
                ua.params.iter_mut().for_each(|expr| {
                    if let Expr_::EnumClassLabel(deref!((cnm_opt, _))) = &mut expr.2 {
                        match &cnm_opt {
                            Some(id) => {
                                if id.name() != sn::hh::MEMOIZE_OPTION {
                                    env.emit_error(NamingError::InvalidMemoizeLabel {
                                        pos: id.pos().clone(),
                                        attr_name: attr_name.to_string(),
                                    })
                                }
                            }
                            None => {
                                // Elaborate.
                                *cnm_opt =
                                    Some(Id(Pos::default(), sn::hh::MEMOIZE_OPTION.to_string()))
                            }
                        }
                    }
                })
            }
        });
        Continue(())
    }
}

#[cfg(test)]
mod tests {

    use std::sync::Arc;

    use nast::Enum_;
    use nast::Pos;
    use nast::UserAttributes;
    use oxidized::namespace_env;
    use oxidized::namespace_env::Mode;
    use oxidized::s_map::SMap;

    use super::*;

    fn make_enum_class_(kind: ClassishKind, enum_: Enum_) -> Class_ {
        Class_ {
            span: Pos::NONE,
            annotation: (),
            mode: file_info::Mode::Mstrict,
            final_: false,
            is_xhp: false,
            has_xhp_keyword: false,
            kind,
            name: Id(Pos::NONE, "Classy".to_string()),
            tparams: vec![],
            extends: vec![],
            uses: vec![],
            xhp_attr_uses: vec![],
            xhp_category: None,
            reqs: vec![],
            implements: vec![],
            consts: vec![],
            typeconsts: vec![],
            vars: vec![],
            methods: vec![],
            xhp_children: vec![],
            xhp_attrs: vec![],
            namespace: Arc::new(namespace_env::Env {
                ns_uses: SMap::default(),
                class_uses: SMap::default(),
                fun_uses: SMap::default(),
                const_uses: SMap::default(),
                name: None,
                mode: Mode::ForTypecheck,
                disable_xhp_element_mangling: false,
            }),
            user_attributes: UserAttributes::default(),
            file_attributes: vec![],
            docs_url: None,
            enum_: Some(enum_),
            doc_comment: None,
            emit_id: None,
            internal: false,
            module: None,
        }
    }

    #[test]
    fn test_enum_class_concrete() {
        let env = Env::default();

        let mut pass = ElabEnumClassPass;

        let mut elem = make_enum_class_(
            ClassishKind::CenumClass(Abstraction::Concrete),
            Enum_ {
                base: Hint(Pos::NONE, Box::new(Hint_::Hmixed)),
                constraint: None,
                includes: vec![],
            },
        );

        elem.transform(&env, &mut pass);
        let Hint(_, hint_) = &mut elem.extends.pop().unwrap();

        assert!(match &mut **hint_ {
            Hint_::Happly(Id(_, cname), bounds) => {
                cname == sn::classes::HH_BUILTIN_ENUM_CLASS
                    && bounds.pop().map_or(false, |Hint(_, hint_)| match *hint_ {
                        Hint_::Happly(Id(_, member_of), mut tparams) => {
                            (member_of == sn::classes::MEMBER_OF)
                                && (tparams.pop().map_or(false, |Hint(_, hint_)| {
                                    matches!(*hint_, Hint_::Hmixed)
                                }))
                                && (tparams.pop().map_or(false, |Hint(_, hint_)| {
                                    matches!(*hint_, Hint_::Happly(..))
                                }))
                        }
                        _ => false,
                    })
            }
            _ => false,
        })
    }

    #[test]
    fn test_enum_class_abstract() {
        let env = Env::default();

        let mut pass = ElabEnumClassPass;

        let mut elem = make_enum_class_(
            ClassishKind::CenumClass(Abstraction::Abstract),
            Enum_ {
                base: Hint(Pos::NONE, Box::new(Hint_::Hmixed)),
                constraint: None,
                includes: vec![],
            },
        );

        elem.transform(&env, &mut pass);
        let Hint(_, hint_) = &mut elem.extends.pop().unwrap();

        assert!(match &mut **hint_ {
            Hint_::Happly(Id(_, cname), bounds) => {
                cname == sn::classes::HH_BUILTIN_ABSTRACT_ENUM_CLASS && bounds.is_empty()
            }
            _ => false,
        })
    }

    #[test]
    fn test_enum() {
        let env = Env::default();

        let mut pass = ElabEnumClassPass;

        let mut elem = make_enum_class_(
            ClassishKind::Cenum,
            Enum_ {
                base: Hint(Pos::NONE, Box::new(Hint_::Hmixed)),
                constraint: None,
                includes: vec![],
            },
        );

        elem.transform(&env, &mut pass);
        let Hint(_, hint_) = &mut elem.extends.pop().unwrap();

        assert!(match &mut **hint_ {
            Hint_::Happly(Id(_, cname), bounds) => {
                cname == sn::classes::HH_BUILTIN_ENUM
                    && bounds
                        .pop()
                        .map_or(false, |Hint(_, hint_)| matches!(*hint_, Hint_::Happly(..)))
            }
            _ => false,
        })
    }
}
