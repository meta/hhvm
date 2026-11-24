// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use naming_special_names_rust as sn;
use nast::Class_;
use nast::ClassConst;
use nast::ConstraintKind;
use nast::Enum_;
use nast::Expr_;
use nast::FunDef;
use nast::HfParamInfo;
use nast::Hint;
use nast::Hint_;
use nast::Id;
use nast::Method_;
use nast::NastShapeInfo;
use nast::ParamKind;
use nast::Pos;
use nast::Tparam;
use nast::Typedef;
use nast::TypedefVisibility;
use nast::UserAttribute;
use nast::UserAttributes;

use crate::prelude::*;

#[derive(Clone, Default, Copy)]
pub struct ElabEverythingSdtPass {
    in_is_as: bool,
    in_enum_class: bool,
    under_no_auto_dynamic: bool,
    under_no_auto_likes: bool,
}

impl ElabEverythingSdtPass {
    fn implicit_sdt(&self, env: &Env) -> bool {
        env.everything_sdt() && !self.under_no_auto_dynamic
    }
}

fn no_auto_dynamic(user_attributes: &UserAttributes) -> bool {
    user_attributes
        .iter()
        .any(|ua| ua.name.name() == sn::user_attributes::NO_AUTO_DYNAMIC)
}

fn no_auto_likes(user_attributes: &UserAttributes) -> bool {
    user_attributes
        .iter()
        .any(|ua| ua.name.name() == sn::user_attributes::NO_AUTO_LIKES)
}

fn no_auto_bound(user_attributes: &UserAttributes) -> bool {
    user_attributes
        .iter()
        .any(|ua| ua.name.name() == sn::user_attributes::NO_AUTO_BOUND)
}

fn wrap_like(hint: Hint) -> Hint {
    let Hint(ref p, _) = hint;
    Hint(p.clone(), Box::new(Hint_::Hlike(hint)))
}

fn wrap_like_mut(hint: &mut Hint) {
    // Steal the contents of `hint`.
    let Hint(pos, hint_) = std::mem::replace(hint, elab_utils::hint::null());
    // Make a new `Hint` from them wrapped in a `Hlike` and write the result
    // back to `hint`.
    *hint = wrap_like(Hint(pos, hint_));
}

fn wrap_supportdyn(Hint(pos, hint_): Hint) -> Hint {
    let wrapped = Hint_::Happly(
        Id(pos.clone(), sn::classes::SUPPORT_DYN.to_string()),
        vec![Hint(pos.clone(), hint_)],
    );
    Hint(pos, Box::new(wrapped))
}

fn wrap_supportdyn_mut(hint: &mut Hint) {
    // Steal the contents of `hint`.
    let h = std::mem::replace(hint, elab_utils::hint::null());
    // Wrap them in a `Happly(\\HH\\supportdyn, ...)`.
    let wrapped = wrap_supportdyn(h);
    // Write the result back into `hint`.
    *hint = wrapped;
}

fn add_support_dynamic_type_attribute_mut(pos: Pos, user_attributes: &mut UserAttributes) {
    // Push a "supports dynamic type" attribute on the front (the order
    // probably doesn't matter but we do it this way to match OCaml).
    user_attributes.insert(
        0,
        UserAttribute {
            name: Id(pos, sn::user_attributes::SUPPORT_DYNAMIC_TYPE.to_string()),
            params: vec![],
        },
    );
}

impl Pass for ElabEverythingSdtPass {
    fn on_ty_fun_def_top_down(&mut self, _env: &Env, fd: &mut FunDef) -> ControlFlow<()> {
        self.under_no_auto_dynamic = no_auto_dynamic(&fd.fun.user_attributes);
        self.under_no_auto_likes = no_auto_likes(&fd.fun.user_attributes);
        Continue(())
    }

    fn on_ty_class__top_down(&mut self, _env: &Env, class: &mut Class_) -> ControlFlow<()> {
        self.in_enum_class = class.kind.is_cenum_class();
        self.under_no_auto_dynamic = no_auto_dynamic(&class.user_attributes);
        Continue(())
    }

    fn on_ty_method__top_down(&mut self, _env: &Env, method: &mut Method_) -> ControlFlow<()> {
        self.under_no_auto_dynamic |= no_auto_dynamic(&method.user_attributes);
        self.under_no_auto_likes = no_auto_likes(&method.user_attributes);
        Continue(())
    }

    fn on_ty_expr__top_down(&mut self, _env: &Env, expr_: &mut Expr_) -> ControlFlow<()> {
        self.in_is_as = matches!(expr_, Expr_::Is(_) | Expr_::As(_));
        Continue(())
    }

    fn on_ty_typedef_top_down(&mut self, _env: &Env, typedef: &mut Typedef) -> ControlFlow<()> {
        self.under_no_auto_dynamic = no_auto_dynamic(&typedef.user_attributes);
        self.under_no_auto_likes = no_auto_likes(&typedef.user_attributes);
        Continue(())
    }

    fn on_ty_hint_bottom_up(&mut self, env: &Env, hint: &mut Hint) -> ControlFlow<()> {
        if !self.implicit_sdt(env) {
            return Continue(());
        }
        match hint {
            Hint(_, deref!((Hint_::Hmixed | Hint_::Hnonnull))) if !self.in_is_as => {
                wrap_supportdyn_mut(hint);
            }
            Hint(
                _,
                deref!(Hint_::Hshape(NastShapeInfo {
                    allows_unknown_fields: true,
                    ..
                })),
            ) => {
                wrap_supportdyn_mut(hint);
            }
            // Return types and inout parameter types are pessimised.
            Hint(_, deref!(Hint_::Hfun(hint_fun))) if !self.under_no_auto_likes => {
                for (p, ty) in std::iter::zip(&hint_fun.param_info, &mut hint_fun.param_tys) {
                    if matches!(
                        p,
                        Some(HfParamInfo {
                            kind: ParamKind::Pinout(_),
                            ..
                        })
                    ) {
                        wrap_like_mut(ty);
                    }
                }

                wrap_like_mut(&mut hint_fun.return_ty);
                wrap_supportdyn_mut(hint);
            }
            _ => (),
        }

        Continue(())
    }

    fn on_ty_fun_def_bottom_up(&mut self, env: &Env, fd: &mut FunDef) -> ControlFlow<()> {
        self.under_no_auto_dynamic = no_auto_dynamic(&fd.fun.user_attributes);
        self.under_no_auto_likes = no_auto_likes(&fd.fun.user_attributes);
        if !self.implicit_sdt(env) {
            return Continue(());
        }

        add_support_dynamic_type_attribute_mut(fd.name.pos().clone(), &mut fd.fun.user_attributes);

        Continue(())
    }

    fn on_ty_tparam_bottom_up(&mut self, env: &Env, tp: &mut Tparam) -> ControlFlow<()> {
        if !self.implicit_sdt(env) || no_auto_bound(&tp.user_attributes) {
            return Continue(());
        }

        // Push a "as supports dynamic mixed" constraint on the front of
        // `tp.constraints` (the order probably doesn't matter but we do it this
        // way to match OCaml).
        tp.constraints.insert(
            0,
            (
                ConstraintKind::ConstraintAs,
                wrap_supportdyn(Hint(tp.name.pos().clone(), Box::new(Hint_::Hmixed))),
            ),
        );

        Continue(())
    }

    fn on_ty_class__bottom_up(&mut self, env: &Env, class: &mut Class_) -> ControlFlow<()> {
        self.under_no_auto_dynamic = no_auto_dynamic(&class.user_attributes);
        if !self.implicit_sdt(env) {
            return Continue(());
        }

        let kind = &class.kind;
        if kind.is_cclass() || kind.is_cinterface() || kind.is_ctrait() {
            add_support_dynamic_type_attribute_mut(
                class.name.pos().clone(),
                &mut class.user_attributes,
            );
        }

        Continue(())
    }

    fn on_fld_class__consts_bottom_up(
        &mut self,
        env: &Env,
        consts: &mut Vec<ClassConst>,
    ) -> ControlFlow<()> {
        if !env.everything_sdt() || !self.in_enum_class {
            return Continue(());
        }

        for c in consts {
            match &mut c.type_ {
                Some(Hint(_, deref!(Hint_::Happly(Id(_, c_member_of), hints))))
                    if c_member_of == sn::classes::MEMBER_OF
                        && matches!(&hints[..], [Hint(_, deref!(Hint_::Happly(_, _))), _]) =>
                {
                    wrap_like_mut(&mut hints[1]);
                    hints[1].0 = hints[0].0.clone(); // Match OCaml.
                }
                _ => {}
            }
        }

        Continue(())
    }

    fn on_ty_enum__bottom_up(&mut self, env: &Env, enum_: &mut Enum_) -> ControlFlow<()> {
        if !env.everything_sdt() || !self.in_enum_class {
            return Continue(());
        }

        wrap_like_mut(&mut enum_.base);

        Continue(())
    }

    fn on_ty_typedef_bottom_up(&mut self, env: &Env, td: &mut Typedef) -> ControlFlow<()> {
        self.under_no_auto_dynamic = no_auto_dynamic(&td.user_attributes);
        self.under_no_auto_likes = no_auto_likes(&td.user_attributes);
        if !self.implicit_sdt(env) {
            return Continue(());
        }

        // If there isn't an "as constraint", and this is not just a type alias, produce a
        // `Happly(\\HH\\supportdyn, Hmixed)` and write it into
        // `td.as_constraint` in-place.
        if td.as_constraint.is_none() && td.vis != TypedefVisibility::Transparent {
            let pos = td.name.pos().clone();
            td.as_constraint = Some(wrap_supportdyn(Hint(pos, Box::new(Hint_::Hmixed))));
        }

        Continue(())
    }
}
