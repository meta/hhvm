// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use oxidized as o;
use pos::Pos;
use pos::ToOxidized;

use super::folded;
use super::shallow;
use super::ty::*;
use crate::reason::Reason;

impl ToOxidized for CeVisibility {
    type Output = o::typing_defs::CeVisibility;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::CeVisibility as O;
        match self {
            CeVisibility::Public => O::Vpublic,
            CeVisibility::Private(v) => O::Vprivate(v.to_oxidized()),
            CeVisibility::Protected(v) => O::Vprotected(v.to_oxidized()),
            CeVisibility::Internal(v) => O::Vinternal(v.to_oxidized()),
            CeVisibility::ProtectedInternal(ty, m) => O::VprotectedInternal {
                class_id: ty.to_oxidized(),
                module__: m.to_oxidized(),
            },
        }
    }
}

impl ToOxidized for UserAttributeParam {
    type Output = o::typing_defs::UserAttributeParam;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::UserAttributeParam as P;
        match self {
            UserAttributeParam::Classname(cn) => P::Classname(cn.to_oxidized()),
            UserAttributeParam::EnumClassLabel(l) => P::EnumClassLabel(l.to_oxidized()),
            UserAttributeParam::String(s) => P::String(s.to_oxidized()),
            UserAttributeParam::Int(i) => P::Int(i.to_oxidized()),
        }
    }
}

impl<P: Pos> ToOxidized for UserAttribute<P> {
    type Output = o::typing_defs::UserAttribute;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::UserAttribute {
            name: self.name.to_oxidized(),
            params: self.params.to_oxidized(),
            raw_val: self.raw_val,
        }
    }
}

impl<R: Reason> ToOxidized for Tparam<R, Ty<R>> {
    type Output = o::typing_defs::Tparam;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::Tparam {
            variance: self.variance,
            name: self.name.to_oxidized(),
            constraints: self
                .constraints
                .into_vec()
                .into_iter()
                .map(|(x, y)| (x, y.to_oxidized()))
                .collect(),
            reified: self.reified,
            user_attributes: self.user_attributes.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for WhereConstraint<Ty<R>> {
    type Output = o::typing_defs::WhereConstraint;

    fn to_oxidized(self) -> Self::Output {
        let WhereConstraint(tvar_ty, kind, as_ty) = self;
        o::typing_defs::WhereConstraint(tvar_ty.to_oxidized(), kind, as_ty.to_oxidized())
    }
}

impl<R: Reason> ToOxidized for ShapeFieldType<R> {
    type Output = o::typing_defs::ShapeFieldType;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::ShapeFieldType {
            optional: self.optional,
            ty: self.ty.to_oxidized(),
        }
    }
}

fn oxidize_shape_field_name<P: Pos>(
    name: TshapeFieldName,
    field_name_pos: ShapeFieldNamePos<P>,
) -> o::typing_defs::TshapeFieldName {
    use ShapeFieldNamePos as SfnPos;
    use o::typing_defs::TshapeFieldName as O;
    match (name, field_name_pos) {
        (TshapeFieldName::TSFregexGroup(x), SfnPos::Simple(p)) => {
            O::TSFregexGroup(o::typing_defs::PosString(p.to_oxidized(), x.to_oxidized()))
        }
        (TshapeFieldName::TSFlitStr(x), SfnPos::Simple(p)) => O::TSFlitStr(
            o::typing_defs::PosByteString(p.to_oxidized(), x.to_oxidized()),
        ),
        (
            TshapeFieldName::TSFregexGroup(_) | TshapeFieldName::TSFlitStr(_),
            SfnPos::ClassConst(_, _),
        ) => panic!("expected ShapeFieldNamePos::Simple"),
        (TshapeFieldName::TSFclassConst(cls, name), SfnPos::ClassConst(p1, p2)) => {
            O::TSFclassConst(
                (p1.to_oxidized(), cls.to_oxidized()),
                o::typing_defs::PosString(p2.to_oxidized(), name.to_oxidized()),
            )
        }
        (TshapeFieldName::TSFclassConst(_, _), SfnPos::Simple(_)) => {
            panic!("expected ShapeFieldNamePos::ClassConst")
        }
    }
}

impl<R: Reason> ToOxidized for Ty_<R> {
    type Output = o::typing_defs::Ty_;

    fn to_oxidized(self) -> Self::Output {
        use o::t_shape_map::TShapeField;
        use o::t_shape_map::TShapeMap;
        use o::typing_defs;
        match self {
            Ty_::Tthis => typing_defs::Ty_::Tthis,
            Ty_::Tapply(box (name, tyl)) => {
                typing_defs::Ty_::Tapply(name.to_oxidized(), tyl.to_oxidized())
            }
            Ty_::Tmixed => typing_defs::Ty_::Tmixed,
            Ty_::Twildcard => typing_defs::Ty_::Twildcard,
            Ty_::Tlike(x) => typing_defs::Ty_::Tlike(x.to_oxidized()),
            Ty_::Tany => typing_defs::Ty_::Tany(o::tany_sentinel::TanySentinel),
            Ty_::Tnonnull => typing_defs::Ty_::Tnonnull,
            Ty_::Tdynamic => typing_defs::Ty_::Tdynamic(None),
            Ty_::Toption(x) => typing_defs::Ty_::Toption(x.to_oxidized()),
            Ty_::Tprim(x) => typing_defs::Ty_::Tprim(x),
            Ty_::Tfun(x) => typing_defs::Ty_::Tfun(x.to_oxidized()),
            Ty_::Ttuple(tuple) => {
                let box TupleType(required, optional, extra) = tuple;
                let required = required.to_oxidized();
                let optional = optional.to_oxidized();
                let extra = match extra {
                    TupleExtra::Tvariadic(variadic) => {
                        typing_defs::TupleExtra::Tvariadic(variadic.to_oxidized())
                    }
                    TupleExtra::Tsplat(splat) => {
                        typing_defs::TupleExtra::Tsplat(splat.to_oxidized())
                    }
                };
                typing_defs::Ty_::Ttuple(typing_defs::TupleType {
                    required,
                    optional,
                    extra,
                })
            }
            Ty_::Tshape(shape) => match *shape {
                ShapeType::Simple(ShapeTypeSimple(shape_kind, shape_field_type_map)) => {
                    let mut shape_fields = TShapeMap::new();
                    for (k, v) in shape_field_type_map.into_iter() {
                        let k = oxidize_shape_field_name(k, v.field_name_pos.clone());
                        shape_fields.insert(TShapeField(k), v.to_oxidized());
                    }
                    let shape_kind = shape_kind.to_oxidized();
                    let shape_origin = typing_defs::TypeOrigin::MissingOrigin;
                    typing_defs::Ty_::Tshape(typing_defs::ShapeType::ShapeSimple(
                        typing_defs::ShapeTypeSimple {
                            origin: shape_origin,
                            unknown_value: shape_kind,
                            fields: shape_fields,
                        },
                    ))
                }
                ShapeType::Splat(elems) => typing_defs::Ty_::Tshape(
                    typing_defs::ShapeType::ShapeSplat(typing_defs::ShapeTypeSplat {
                        ss_elems: elems.into_iter().map(|t| t.to_oxidized()).collect(),
                    }),
                ),
            },
            Ty_::Tgeneric(name) => typing_defs::Ty_::Tgeneric(name.to_oxidized()),
            Ty_::Tunion(x) => typing_defs::Ty_::Tunion(x.to_oxidized()),
            Ty_::Tintersection(x) => typing_defs::Ty_::Tintersection(x.to_oxidized()),
            Ty_::TvecOrDict(box (tk, tv)) => {
                typing_defs::Ty_::TvecOrDict(tk.to_oxidized(), tv.to_oxidized())
            }
            Ty_::Taccess(x) => typing_defs::Ty_::Taccess(x.to_oxidized()),
            Ty_::Trefinement(tr) => {
                typing_defs::Ty_::Trefinement(tr.ty.to_oxidized(), tr.refinement.to_oxidized())
            }
            Ty_::TclassPtr(x) => typing_defs::Ty_::TclassPtr(x.to_oxidized()),
        }
    }
}

impl<R: Reason> ToOxidized for ClassRefinement<Ty<R>> {
    type Output = o::typing_defs::ClassRefinement;
    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::ClassRefinement {
            cr_consts: self.consts.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for RefinedConstBound<Ty<R>> {
    type Output = o::typing_defs::RefinedConstBound;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::RefinedConstBound::*;
        match self {
            Self::Exact(ty) => TRexact(ty.to_oxidized()),
            Self::Loose(bounds) => TRloose(bounds.to_oxidized()),
        }
    }
}

impl<R: Reason> ToOxidized for RefinedConst<Ty<R>> {
    type Output = o::typing_defs::RefinedConst;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::RefinedConst {
            bound: self.bound.to_oxidized(),
            is_ctx: self.is_ctx,
        }
    }
}

impl<R: Reason> ToOxidized for RefinedConstBounds<Ty<R>> {
    type Output = o::typing_defs::RefinedConstBounds;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::RefinedConstBounds {
            lower: self.lower.to_oxidized(),
            upper: self.upper.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for TaccessType<R, Ty<R>> {
    type Output = o::typing_defs::TaccessType;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::TaccessType(self.ty.to_oxidized(), self.type_const.to_oxidized())
    }
}

impl<R: Reason> ToOxidized for FunImplicitParams<R, Ty<R>> {
    type Output = o::typing_defs::FunImplicitParams;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::FunImplicitParams {
            capability: match self.capability {
                Capability::CapDefaults(p) => {
                    o::typing_defs::Capability::CapDefaults(p.to_oxidized())
                }
                Capability::CapTy(ty) => o::typing_defs::Capability::CapTy(ty.to_oxidized()),
            },
        }
    }
}

impl<R: Reason> ToOxidized for FunType<R, Ty<R>> {
    type Output = o::typing_defs::FunType;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::FunType {
            tparams: self.tparams.to_oxidized(),
            where_constraints: self.where_constraints.to_oxidized(),
            params: self.params.to_oxidized(),
            implicit_params: self.implicit_params.to_oxidized(),
            ret: self.ret.to_oxidized(),
            flags: self.flags,
            instantiated: self.instantiated,
        }
    }
}

impl<R: Reason> ToOxidized for FunParam<R, Ty<R>> {
    type Output = o::typing_defs::FunParam;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::FunParam {
            pos: self.pos.to_oxidized(),
            name: self.name.to_oxidized(),
            type_: self.ty.to_oxidized(),
            flags: self.flags,
            def_value: self.def_value,
        }
    }
}

impl ToOxidized for XhpEnumValue {
    type Output = o::ast_defs::XhpEnumValue;

    fn to_oxidized(self) -> Self::Output {
        use o::ast_defs::XhpEnumValue as O;
        match self {
            Self::XEVInt(i) => O::XEVInt(i),
            Self::XEVString(s) => O::XEVString(s.to_oxidized()),
        }
    }
}

impl ToOxidized for ConstValue {
    type Output = o::typing_defs::ConstValue;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::ConstValue as O;
        match self {
            Self::CVInt(n) => O::CVInt(n),
            Self::CVLargeInt(s) => O::CVLargeInt(s),
            Self::CVString(s) => O::CVString(s),
            Self::CVNameof(s) => O::CVNameof(s),
            Self::CVLabel => O::CVLabel,
            Self::CVAbsent => O::CVAbsent,
            Self::CVConstAccess(e, m) => O::CVConstAccess(e, m),
        }
    }
}

impl ToOxidized for ClassConstFrom {
    type Output = o::typing_defs::ClassConstFrom;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::ClassConstFrom as O;
        match self {
            Self::Self_ => O::Self_,
            Self::From(ty) => O::From(ty.to_oxidized()),
        }
    }
}

impl ToOxidized for ClassConstRef {
    type Output = o::typing_defs::ClassConstRef;

    fn to_oxidized(self) -> Self::Output {
        let ClassConstRef(class, symbol) = self;
        o::typing_defs::ClassConstRef(class.to_oxidized(), symbol.to_oxidized())
    }
}

impl<R: Reason> ToOxidized for AbstractTypeconst<R> {
    type Output = o::typing_defs::AbstractTypeconst;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::AbstractTypeconst {
            as_constraint: self.as_constraint.to_oxidized(),
            super_constraint: self.super_constraint.to_oxidized(),
            default: self.default.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for ConcreteTypeconst<R> {
    type Output = o::typing_defs::ConcreteTypeconst;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::ConcreteTypeconst {
            tc_type: self.ty.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for Typeconst<R> {
    type Output = o::typing_defs::Typeconst;

    fn to_oxidized(self) -> Self::Output {
        use o::typing_defs::Typeconst as O;
        match self {
            Self::TCAbstract(x) => O::TCAbstract(x.to_oxidized()),
            Self::TCConcrete(x) => O::TCConcrete(x.to_oxidized()),
        }
    }
}

impl<R: Reason> ToOxidized for EnumType<R> {
    type Output = o::typing_defs::EnumType;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::EnumType {
            base: self.base.to_oxidized(),
            constraint: self.constraint.to_oxidized(),
            includes: self.includes.to_oxidized(),
        }
    }
}

impl<P: Pos> ToOxidized for Enforceable<P> {
    type Output = (o::pos::Pos, bool);

    fn to_oxidized(self) -> Self::Output {
        self.0
            .map_or_else(|| (o::pos::Pos::NONE, false), |x| (x.to_oxidized(), true))
    }
}

impl<R: Reason> ToOxidized for folded::SubstContext<R> {
    type Output = o::decl_defs::SubstContext;

    fn to_oxidized(self) -> Self::Output {
        o::decl_defs::SubstContext {
            subst: self.subst.0.to_oxidized(),
            class_context: self.class_context.to_oxidized(),
            from_req_extends: self.from_req_extends,
        }
    }
}

impl<R: Reason> ToOxidized for folded::TypeConst<R> {
    type Output = o::typing_defs::TypeconstType;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::TypeconstType {
            synthesized: self.is_synthesized,
            concretized: self.is_concretized,
            is_ctx: self.is_ctx,
            enforceable: self
                .enforceable
                .0
                .map_or_else(|| (o::pos::Pos::NONE, false), |x| (x.to_oxidized(), true)),
            reifiable: self.reifiable.to_oxidized(),
            origin: self.origin.to_oxidized(),
            kind: self.kind.to_oxidized(),
            name: self.name.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for folded::ClassConst<R> {
    type Output = o::typing_defs::ClassConst;

    fn to_oxidized(self) -> Self::Output {
        o::typing_defs::ClassConst {
            synthesized: self.is_synthesized,
            abstract_: self.kind,
            origin: self.origin.to_oxidized(),
            refs: self.refs.to_oxidized(),
            type_: self.ty.to_oxidized(),
            pos: self.pos.to_oxidized(),
            is_shape_key: self.is_shape_key,
            value: self.value.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for folded::Requirement<R> {
    type Output = o::decl_defs::Requirement;

    fn to_oxidized(self) -> Self::Output {
        o::decl_defs::Requirement(self.pos.to_oxidized(), self.ty.to_oxidized())
    }
}

impl<R: Reason> ToOxidized for folded::ConstraintRequirement<R> {
    type Output = o::decl_defs::ConstraintRequirement;

    fn to_oxidized(self) -> Self::Output {
        use o::decl_defs::ConstraintRequirement as O;
        match self {
            Self::CREqual(r) => O::CREqual(r.to_oxidized()),
            Self::CRSubtype(r) => O::CRSubtype(r.to_oxidized()),
        }
    }
}

impl ToOxidized for folded::Constructor {
    type Output = (Option<o::decl_defs::Element>, ConsistentKind);

    fn to_oxidized(self) -> Self::Output {
        (self.elt.to_oxidized(), self.consistency)
    }
}

impl<R: Reason> ToOxidized for folded::FoldedClass<R> {
    type Output = o::decl_defs::DeclClassType;

    fn to_oxidized(self) -> Self::Output {
        let abstract_ = self.is_abstract();
        let need_init = self.has_concrete_constructor();
        // Destructure to help ensure we convert every field.
        let Self {
            name,
            pos,
            kind,
            is_final,
            is_const,
            is_internal,
            tests_bypass_visibility,
            allow_unchecked_enum_values,
            is_xhp,
            has_xhp_keyword,
            support_dynamic_type,
            module,
            is_module_level_trait,
            tparams,
            substs,
            ancestors,
            props,
            static_props,
            methods,
            static_methods,
            consts,
            type_consts,
            xhp_enum_values,
            xhp_marked_empty,
            constructor,
            deferred_init_members,
            req_ancestors,
            req_ancestors_extends,
            req_constraints_ancestors,
            extends,
            sealed_whitelist,
            xhp_attr_deps,
            enum_type,
            decl_errors,
            docs_url,
            allow_multiple_instantiations,
            package,
        } = self;
        o::decl_defs::DeclClassType {
            name: name.to_oxidized(),
            pos: pos.to_oxidized(),
            kind,
            abstract_,
            final_: is_final,
            const_: is_const,
            internal: is_internal,
            is_xhp,
            has_xhp_keyword,
            support_dynamic_type,
            module: module.map(|m| {
                let (pos, id) = m.to_oxidized();
                o::ast_defs::Id(pos, id)
            }),
            is_module_level_trait,
            tparams: tparams.to_oxidized(),
            substs: substs.to_oxidized(),
            ancestors: ancestors.to_oxidized(),
            props: props.to_oxidized(),
            sprops: static_props.to_oxidized(),
            methods: methods.to_oxidized(),
            smethods: static_methods.to_oxidized(),
            consts: consts.to_oxidized(),
            typeconsts: type_consts.to_oxidized(),
            xhp_enum_values: xhp_enum_values.to_oxidized(),
            xhp_marked_empty,
            construct: constructor.to_oxidized(),
            need_init,
            deferred_init_members: deferred_init_members.to_oxidized(),
            req_ancestors: req_ancestors.to_oxidized(),
            req_ancestors_extends: req_ancestors_extends.to_oxidized(),
            req_constraints_ancestors: req_constraints_ancestors.to_oxidized(),
            extends: extends.to_oxidized(),
            sealed_whitelist: sealed_whitelist.to_oxidized(),
            xhp_attr_deps: xhp_attr_deps.to_oxidized(),
            enum_type: enum_type.to_oxidized(),
            decl_errors: decl_errors.to_oxidized(),
            docs_url,
            allow_multiple_instantiations,
            package,
            tests_bypass_visibility,
            allow_unchecked_enum_values,
        }
    }
}

impl ToOxidized for folded::FoldedElement {
    type Output = o::decl_defs::Element;

    fn to_oxidized(self) -> Self::Output {
        o::decl_defs::Element {
            origin: self.origin.to_oxidized(),
            visibility: self.visibility.to_oxidized(),
            deprecated: self
                .deprecated
                .map(|x| String::from_utf8_lossy(x.as_bytes()).to_string()),
            flags: self.flags,
            sealed_allowlist: self.sealed_allowlist.to_oxidized(),
            overlapping_tparams: self.overlapping_tparams.to_oxidized(),
            package_requirement: self.package_requirement,
        }
    }
}

impl<P: ToOxidized<Output = o::pos::Pos>> ToOxidized for crate::decl_error::DeclError<P> {
    type Output = o::decl_defs::DeclError;

    fn to_oxidized(self) -> Self::Output {
        use o::decl_defs::DeclError;
        match self {
            Self::WrongExtendKind {
                pos,
                kind,
                name,
                parent_pos,
                parent_kind,
                parent_name,
            } => DeclError::WrongExtendKind {
                pos: pos.to_oxidized(),
                kind,
                name: name.to_oxidized(),
                parent_pos: parent_pos.to_oxidized(),
                parent_kind,
                parent_name: parent_name.to_oxidized(),
            },
            Self::WrongUseKind {
                pos,
                name,
                parent_pos,
                parent_name,
            } => DeclError::WrongUseKind {
                pos: pos.to_oxidized(),
                name: name.to_oxidized(),
                parent_pos: parent_pos.to_oxidized(),
                parent_name: parent_name.to_oxidized(),
            },
            Self::CyclicClassDef(pos, stack) => {
                let mut s = o::s_set::SSet::new();
                for x in stack.into_iter() {
                    s.insert(x.to_oxidized());
                }
                DeclError::CyclicClassDef {
                    pos: pos.to_oxidized(),
                    stack: s,
                }
            }
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ShallowMethod<R> {
    type Output = o::shallow_decl_defs::ShallowMethod;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            name,
            ty,
            visibility,
            deprecated,
            flags,
            attributes,
            package_requirement,
        } = self;
        o::shallow_decl_defs::ShallowMethod {
            name: name.to_oxidized(),
            type_: ty.to_oxidized(),
            visibility,
            deprecated: deprecated.map(|s| String::from_utf8_lossy(s.as_bytes()).to_string()),
            flags: o::method_flags::MethodFlags::from_bits_truncate(flags.bits()),
            attributes: attributes.to_oxidized(),
            package_requirement,
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ShallowProp<R> {
    type Output = o::shallow_decl_defs::ShallowProp;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            name,
            xhp_attr,
            ty,
            visibility,
            flags,
        } = self;
        o::shallow_decl_defs::ShallowProp {
            name: name.to_oxidized(),
            xhp_attr,
            type_: ty.to_oxidized(),
            visibility,
            flags: o::prop_flags::PropFlags::from_bits_truncate(flags.bits()),
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ShallowClassConst<R> {
    type Output = o::shallow_decl_defs::ShallowClassConst;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            kind,
            name,
            ty,
            refs,
            value,
            is_shape_key,
            const_value,
        } = self;
        o::shallow_decl_defs::ShallowClassConst {
            abstract_: kind,
            name: name.to_oxidized(),
            type_: ty.to_oxidized(),
            refs: refs.to_oxidized(),
            value,
            is_shape_key,
            const_value: const_value.to_oxidized(),
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ShallowTypeconst<R> {
    type Output = o::shallow_decl_defs::ShallowTypeconst;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            name,
            kind,
            enforceable,
            reifiable,
            is_ctx,
        } = self;
        o::shallow_decl_defs::ShallowTypeconst {
            name: name.to_oxidized(),
            kind: kind.to_oxidized(),
            enforceable: enforceable.to_oxidized(),
            reifiable: reifiable.to_oxidized(),
            is_ctx,
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ClassDecl<R> {
    type Output = o::shallow_decl_defs::ClassDecl;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            mode,
            is_final,
            is_abstract,
            is_internal,
            is_xhp,
            has_xhp_keyword,
            kind,
            module,
            name,
            tparams,
            extends,
            uses,
            xhp_attr_uses,
            xhp_enum_values,
            xhp_marked_empty,
            req_extends,
            req_implements,
            req_constraints,
            implements,
            support_dynamic_type,
            consts,
            typeconsts,
            props,
            static_props,
            constructor,
            static_methods,
            methods,
            user_attributes,
            enum_type,
            docs_url,
            package,
        } = self;

        o::shallow_decl_defs::ClassDecl {
            mode,
            final_: is_final,
            abstract_: is_abstract,
            is_xhp,
            internal: is_internal,
            has_xhp_keyword,
            kind,
            module: module.map(|m| {
                let (pos, id) = m.to_oxidized();
                o::ast_defs::Id(pos, id)
            }),
            name: name.to_oxidized(),
            tparams: tparams.to_oxidized(),
            extends: extends.to_oxidized(),
            uses: uses.to_oxidized(),
            xhp_attr_uses: xhp_attr_uses.to_oxidized(),
            xhp_enum_values: xhp_enum_values.to_oxidized(),
            xhp_marked_empty,
            req_extends: req_extends.to_oxidized(),
            req_implements: req_implements.to_oxidized(),
            req_constraints: req_constraints.to_oxidized(),
            implements: implements.to_oxidized(),
            support_dynamic_type,
            consts: consts.to_oxidized(),
            typeconsts: typeconsts.to_oxidized(),
            props: props.to_oxidized(),
            sprops: static_props.to_oxidized(),
            constructor: constructor.to_oxidized(),
            static_methods: static_methods.to_oxidized(),
            methods: methods.to_oxidized(),
            user_attributes: user_attributes.to_oxidized(),
            enum_type: enum_type.to_oxidized(),
            docs_url,
            package,
        }
    }
}

impl<R: Reason> ToOxidized for shallow::FunDecl<R> {
    type Output = o::shallow_decl_defs::FunDecl;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            deprecated,
            module,
            package,
            internal,
            ty,
            pos,
            php_std_lib,
            support_dynamic_type,
            no_auto_dynamic,
            no_auto_likes,
            gated_by_feature_flag,
            package_requirement,
        } = self;
        o::shallow_decl_defs::FunDecl {
            deprecated: deprecated.map(|s| String::from_utf8_lossy(s.as_bytes()).to_string()),
            internal,
            type_: ty.to_oxidized(),
            pos: pos.to_oxidized(),
            php_std_lib,
            support_dynamic_type,
            no_auto_dynamic,
            no_auto_likes,
            gated_by_feature_flag,
            module: module.map(|m| {
                let (pos, id) = m.to_oxidized();
                o::ast_defs::Id(pos, id)
            }),
            package,
            package_requirement,
        }
    }
}

impl<R: Reason> ToOxidized for shallow::TypedefDecl<R> {
    type Output = o::shallow_decl_defs::TypedefDecl;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            module,
            pos,
            tparams,
            as_constraint,
            super_constraint,
            type_assignment,
            is_ctx,
            attributes,
            internal,
            docs_url,
            package,
        } = self;
        o::shallow_decl_defs::TypedefDecl {
            module: module.map(|m| {
                let (pos, id) = m.to_oxidized();
                o::ast_defs::Id(pos, id)
            }),
            pos: pos.to_oxidized(),
            tparams: tparams.to_oxidized(),
            as_constraint: as_constraint.to_oxidized(),
            super_constraint: super_constraint.to_oxidized(),
            type_assignment: type_assignment.to_oxidized(),
            is_ctx,
            attributes: attributes.to_oxidized(),
            internal,
            docs_url,
            package,
        }
    }
}

impl<R: Reason> ToOxidized for DeclConstraintRequirement<R> {
    type Output = o::shallow_decl_defs::DeclConstraintRequirement;

    fn to_oxidized(self) -> Self::Output {
        use o::shallow_decl_defs::DeclConstraintRequirement as O;
        match self {
            DeclConstraintRequirement::DCREqual(ty) => O::DCREqual(ty.to_oxidized()),
            DeclConstraintRequirement::DCRSubtype(ty) => O::DCRSubtype(ty.to_oxidized()),
        }
    }
}

impl<R: Reason> ToOxidized for TypedefTypeAssignment<R> {
    type Output = o::typing_defs::TypedefTypeAssignment;

    fn to_oxidized(self) -> Self::Output {
        use o::decl_defs::TypedefTypeAssignment as O;
        match self {
            TypedefTypeAssignment::SimpleTypeDef(vis, ty) => {
                O::SimpleTypeDef(vis, ty.to_oxidized())
            }
            TypedefTypeAssignment::CaseType(variant, variants) => {
                O::CaseType(variant.to_oxidized(), variants.to_oxidized())
            }
        }
    }
}

impl<R: Reason> ToOxidized for TypedefCaseTypeVariant<R> {
    type Output = o::typing_defs::TypedefCaseTypeVariant;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            hint,
            where_constraints,
        } = self;
        o::typing_defs::TypedefCaseTypeVariant(hint.to_oxidized(), where_constraints.to_oxidized())
    }
}

impl<R: Reason> ToOxidized for shallow::ConstDecl<R> {
    type Output = o::shallow_decl_defs::ConstDecl;

    fn to_oxidized(self) -> Self::Output {
        let Self {
            pos,
            ty,
            value,
            package,
        } = self;
        o::shallow_decl_defs::ConstDecl {
            pos: pos.to_oxidized(),
            type_: ty.to_oxidized(),
            value,
            package,
        }
    }
}

impl<R: Reason> ToOxidized for shallow::ModuleDecl<R> {
    type Output = o::shallow_decl_defs::ModuleDecl;

    fn to_oxidized(self) -> Self::Output {
        let Self { pos } = self;
        o::shallow_decl_defs::ModuleDecl {
            mdt_pos: pos.to_oxidized(),
        }
    }
}
