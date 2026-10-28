// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::hash::Hash;

use oxidized as o;
use pos::Pos;

use super::ty::DeclConstraintRequirement;
use super::ty::TypedefCaseTypeVariant;
use crate::decl;
use crate::decl::Ty;
use crate::decl::Ty_;
use crate::decl::folded;
use crate::decl::shallow;
use crate::decl::ty;
use crate::decl::ty::TypedefTypeAssignment;
use crate::reason::Reason;

#[inline]
fn slice<T: Into<U>, U: From<T>>(items: Vec<T>) -> Box<[U]> {
    items.into_iter().map(Into::into).collect()
}

#[inline]
fn map_k<K1, K2, O>(items: BTreeSet<K1>) -> O
where
    K1: Into<K2>,
    K2: Hash + Eq,
    O: FromIterator<K2>,
{
    items.into_iter().map(Into::into).collect()
}

#[inline]
fn map_kv<K1, V1, K2, V2, O>(items: BTreeMap<K1, V1>) -> O
where
    K1: Into<K2>,
    K2: Hash + Eq,
    V1: Into<V2>,
    O: FromIterator<(K2, V2)>,
{
    items
        .into_iter()
        .map(|(k, v)| (k.into(), v.into()))
        .collect()
}

impl From<o::ast_defs::XhpEnumValue> for ty::XhpEnumValue {
    fn from(x: o::ast_defs::XhpEnumValue) -> Self {
        use o::ast_defs::XhpEnumValue as O;
        match x {
            O::XEVInt(i) => Self::XEVInt(i),
            O::XEVString(s) => Self::XEVString(s.into()),
        }
    }
}

impl From<o::typing_defs::CeVisibility> for ty::CeVisibility {
    fn from(x: o::typing_defs::CeVisibility) -> Self {
        use o::typing_defs::CeVisibility as O;
        match x {
            O::Vpublic => Self::Public,
            O::Vprivate(s) => Self::Private(s.into()),
            O::Vprotected(s) => Self::Protected(s.into()),
            O::Vinternal(s) => Self::Internal(s.into()),
            O::VprotectedInternal { class_id, module__ } => {
                Self::ProtectedInternal(class_id.into(), module__.into())
            }
        }
    }
}

fn tshape_field_name_from_decl<P: Pos>(
    x: o::typing_defs::TshapeFieldName,
) -> (ty::ShapeFieldNamePos<P>, ty::TshapeFieldName) {
    use o::typing_defs_core::TshapeFieldName as O;
    use ty::ShapeFieldNamePos as SfnPos;
    use ty::TshapeFieldName;
    match x {
        O::TSFregexGroup(pos_id) => (
            SfnPos::Simple((&pos_id.0).into()),
            TshapeFieldName::TSFregexGroup(pos_id.1.into()),
        ),
        O::TSFlitStr(pos_bytes) => (
            SfnPos::Simple(pos_bytes.0.into()),
            TshapeFieldName::TSFlitStr(pos_bytes.1.into()),
        ),
        O::TSFclassConst(pos_id1, pos_id2) => (
            SfnPos::ClassConst(pos_id1.0.into(), pos_id2.0.into()),
            TshapeFieldName::TSFclassConst(pos_id1.1.into(), pos_id2.1.into()),
        ),
    }
}

impl From<o::typing_defs::UserAttributeParam> for ty::UserAttributeParam {
    fn from(attr: o::typing_defs::UserAttributeParam) -> Self {
        use o::typing_defs::UserAttributeParam as UAP;
        match attr {
            UAP::Classname(cn) => Self::Classname(cn.into()),
            UAP::EnumClassLabel(l) => Self::EnumClassLabel(l.into()),
            UAP::String(s) => Self::String(s.into()),
            UAP::Int(i) => Self::Int(i.into()),
        }
    }
}

impl<P: Pos> From<o::typing_defs::UserAttribute> for ty::UserAttribute<P> {
    fn from(attr: o::typing_defs::UserAttribute) -> Self {
        Self {
            name: attr.name.into(),
            params: slice(attr.params),
            raw_val: attr.raw_val,
        }
    }
}

impl<R: Reason> From<o::typing_defs::Tparam> for ty::Tparam<R, Ty<R>> {
    fn from(tparam: o::typing_defs::Tparam) -> Self {
        Self {
            variance: tparam.variance,
            name: tparam.name.into(),
            constraints: tparam
                .constraints
                .into_iter()
                .map(|(kind, ty)| (kind, ty.into()))
                .collect(),
            reified: tparam.reified,
            user_attributes: slice(tparam.user_attributes),
        }
    }
}

impl<R: Reason> From<o::typing_defs::WhereConstraint> for ty::WhereConstraint<Ty<R>> {
    fn from(x: o::typing_defs::WhereConstraint) -> Self {
        Self(x.0.into(), x.1, x.2.into())
    }
}

fn decl_shape_field_type<R: Reason>(
    field_name_pos: ty::ShapeFieldNamePos<R::Pos>,
    sft: o::typing_defs::ShapeFieldType,
) -> ty::ShapeFieldType<R> {
    ty::ShapeFieldType {
        field_name_pos,
        optional: sft.optional,
        ty: sft.ty.into(),
    }
}

impl<R: Reason> From<o::typing_defs::Ty> for Ty<R> {
    fn from(ty: o::typing_defs::Ty) -> Self {
        use Ty_::*;
        use o::typing_defs_core;
        use o::typing_defs_core::TupleExtra;
        let reason = R::from(ty.0);
        let ty_ = match *ty.1 {
            typing_defs_core::Ty_::Tthis => Tthis,
            typing_defs_core::Ty_::Tapply(pos_id, tys) => {
                Tapply(Box::new((pos_id.into(), slice(tys))))
            }
            typing_defs_core::Ty_::Tmixed => Tmixed,
            typing_defs_core::Ty_::Twildcard => Twildcard,
            typing_defs_core::Ty_::Tlike(ty) => Tlike(ty.into()),
            typing_defs_core::Ty_::Tany(_) => Tany,
            typing_defs_core::Ty_::Tnonnull => Tnonnull,
            typing_defs_core::Ty_::Tdynamic(_) => Tdynamic,
            typing_defs_core::Ty_::Toption(ty) => Toption(ty.into()),
            typing_defs_core::Ty_::Tprim(prim) => Tprim(prim),
            typing_defs_core::Ty_::Tfun(ft) => Tfun(Box::new(ft.into())),
            typing_defs_core::Ty_::Ttuple(typing_defs_core::TupleType {
                required,
                optional,
                extra,
            }) => {
                let extra = match extra {
                    TupleExtra::Tvariadic(variadic) => ty::TupleExtra::Tvariadic(variadic.into()),
                    TupleExtra::Tsplat(splat) => ty::TupleExtra::Tsplat(splat.into()),
                };
                Ttuple(Box::new(ty::TupleType(
                    slice(required),
                    slice(optional),
                    extra,
                )))
            }
            typing_defs_core::Ty_::Tshape(typing_defs_core::ShapeType::ShapeSimple(
                typing_defs_core::ShapeTypeSimple {
                    origin: _,
                    unknown_value: kind,
                    fields,
                },
            )) => Tshape(Box::new(ty::ShapeType::Simple(ty::ShapeTypeSimple(
                kind.into(),
                fields
                    .into_iter()
                    .map(|(name, ty)| {
                        let (field_name_pos, name) = tshape_field_name_from_decl(name.0);
                        (name, decl_shape_field_type(field_name_pos, ty))
                    })
                    .collect(),
            )))),
            typing_defs_core::Ty_::Tshape(typing_defs_core::ShapeType::ShapeSplat(
                typing_defs_core::ShapeTypeSplat { ss_elems },
            )) => Tshape(Box::new(ty::ShapeType::Splat(
                ss_elems.into_iter().map(Into::into).collect(),
            ))),
            typing_defs_core::Ty_::Trefinement(ty, cr) => {
                Trefinement(Box::new(decl::TrefinementType {
                    ty: ty.into(),
                    refinement: ty::ClassRefinement {
                        consts: map_kv(cr.cr_consts),
                    },
                }))
            }
            typing_defs_core::Ty_::Tgeneric(pos_id) => Tgeneric(pos_id.into()),
            typing_defs_core::Ty_::Tunion(tys) => Tunion(slice(tys)),
            typing_defs_core::Ty_::Tintersection(tys) => Tintersection(slice(tys)),
            typing_defs_core::Ty_::TvecOrDict(ty1, ty2) => {
                TvecOrDict(Box::new((ty1.into(), ty2.into())))
            }
            typing_defs_core::Ty_::Taccess(taccess_type) => Taccess(Box::new(taccess_type.into())),
            typing_defs_core::Ty_::TclassPtr(class_type) => TclassPtr(class_type.into()),
        };
        Ty::new(reason, ty_)
    }
}

impl<R: Reason> From<o::typing_defs::RefinedConst> for ty::RefinedConst<Ty<R>> {
    fn from(rc: o::typing_defs::RefinedConst) -> Self {
        Self {
            bound: rc.bound.into(),
            is_ctx: rc.is_ctx,
        }
    }
}

impl<R: Reason> From<o::typing_defs::RefinedConstBound> for ty::RefinedConstBound<Ty<R>> {
    fn from(ctr: o::typing_defs::RefinedConstBound) -> Self {
        use o::typing_defs::RefinedConstBound::*;
        match ctr {
            TRexact(ty) => Self::Exact(ty.into()),
            TRloose(bounds) => Self::Loose(bounds.into()),
        }
    }
}

impl<R: Reason> From<o::typing_defs::RefinedConstBounds> for ty::RefinedConstBounds<Ty<R>> {
    fn from(bounds: o::typing_defs::RefinedConstBounds) -> Self {
        Self {
            lower: slice(bounds.lower),
            upper: slice(bounds.upper),
        }
    }
}

impl<R: Reason> From<o::typing_defs::TaccessType> for ty::TaccessType<R, Ty<R>> {
    fn from(taccess_type: o::typing_defs::TaccessType) -> Self {
        Self {
            ty: taccess_type.0.into(),
            type_const: taccess_type.1.into(),
        }
    }
}

impl<R: Reason> From<o::typing_defs::Capability> for ty::Capability<R, Ty<R>> {
    fn from(cap: o::typing_defs::Capability) -> Self {
        use o::typing_defs_core::Capability as O;
        match cap {
            O::CapDefaults(pos) => Self::CapDefaults(pos.into()),
            O::CapTy(ty) => Self::CapTy(ty.into()),
        }
    }
}

impl<R: Reason> From<o::typing_defs::FunImplicitParams> for ty::FunImplicitParams<R, Ty<R>> {
    fn from(x: o::typing_defs::FunImplicitParams) -> Self {
        Self {
            capability: x.capability.into(),
        }
    }
}

impl<R: Reason> From<o::typing_defs::FunType> for ty::FunType<R, Ty<R>> {
    fn from(ft: o::typing_defs::FunType) -> Self {
        Self {
            tparams: slice(ft.tparams),
            where_constraints: slice(ft.where_constraints),
            params: slice(ft.params),
            implicit_params: ft.implicit_params.into(),
            ret: ft.ret.into(),
            flags: ft.flags,
            instantiated: ft.instantiated,
        }
    }
}

impl<R: Reason> From<o::typing_defs_core::FunParam> for ty::FunParam<R, Ty<R>> {
    fn from(fp: o::typing_defs_core::FunParam) -> Self {
        Self {
            pos: fp.pos.into(),
            name: fp.name.map(Into::into),
            ty: fp.type_.into(),
            flags: fp.flags,
            def_value: fp.def_value,
        }
    }
}

impl From<o::typing_defs::ClassConstFrom> for ty::ClassConstFrom {
    fn from(x: o::typing_defs::ClassConstFrom) -> Self {
        use o::typing_defs::ClassConstFrom as O;
        match x {
            O::Self_ => Self::Self_,
            O::From(s) => Self::From(s.into()),
        }
    }
}

impl From<o::typing_defs::ConstValue> for ty::ConstValue {
    fn from(x: o::typing_defs::ConstValue) -> Self {
        use o::typing_defs::ConstValue as O;
        match x {
            O::CVInt(n) => Self::CVInt(n),
            O::CVLargeInt(s) => Self::CVLargeInt(s),
            O::CVString(s) => Self::CVString(s),
            O::CVNameof(s) => Self::CVNameof(s),
            O::CVLabel => Self::CVLabel,
            O::CVAbsent => Self::CVAbsent,
            O::CVConstAccess(e, m) => Self::CVConstAccess(e, m),
        }
    }
}

impl From<o::typing_defs::ClassConstRef> for ty::ClassConstRef {
    fn from(x: o::typing_defs::ClassConstRef) -> Self {
        Self(x.0.into(), x.1.into())
    }
}

impl<R: Reason> From<o::typing_defs::AbstractTypeconst> for ty::AbstractTypeconst<R> {
    fn from(x: o::typing_defs::AbstractTypeconst) -> Self {
        Self {
            as_constraint: x.as_constraint.map(Into::into),
            super_constraint: x.super_constraint.map(Into::into),
            default: x.default.map(Into::into),
        }
    }
}

impl<R: Reason> From<o::typing_defs::ConcreteTypeconst> for ty::ConcreteTypeconst<R> {
    fn from(x: o::typing_defs::ConcreteTypeconst) -> Self {
        Self {
            ty: x.tc_type.into(),
        }
    }
}

impl<R: Reason> From<o::typing_defs::Typeconst> for ty::Typeconst<R> {
    fn from(x: o::typing_defs::Typeconst) -> Self {
        use o::typing_defs::Typeconst as O;
        match x {
            O::TCAbstract(atc) => Self::TCAbstract(atc.into()),
            O::TCConcrete(ctc) => Self::TCConcrete(ctc.into()),
        }
    }
}

impl<R: Reason> From<o::typing_defs::EnumType> for ty::EnumType<R> {
    fn from(x: o::typing_defs::EnumType) -> Self {
        Self {
            base: x.base.into(),
            constraint: x.constraint.map(Into::into),
            includes: slice(x.includes),
        }
    }
}

impl<P: Pos> From<(o::pos::Pos, bool)> for ty::Enforceable<P> {
    fn from((pos, is_enforceable): (o::pos::Pos, bool)) -> Self {
        if is_enforceable {
            Self(Some(pos.into()))
        } else {
            Self(None)
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ShallowClassConst> for shallow::ShallowClassConst<R> {
    fn from(scc: o::shallow_decl_defs::ShallowClassConst) -> Self {
        Self {
            kind: scc.abstract_,
            name: scc.name.into(),
            ty: scc.type_.into(),
            refs: slice(scc.refs),
            value: scc.value,
            is_shape_key: scc.is_shape_key,
            const_value: scc.const_value.into(),
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ShallowTypeconst> for shallow::ShallowTypeconst<R> {
    fn from(stc: o::shallow_decl_defs::ShallowTypeconst) -> Self {
        Self {
            name: stc.name.into(),
            kind: stc.kind.into(),
            enforceable: <ty::Enforceable<R::Pos>>::from(stc.enforceable),
            reifiable: stc.reifiable.map(Into::into),
            is_ctx: stc.is_ctx,
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ShallowMethod> for shallow::ShallowMethod<R> {
    fn from(sm: o::shallow_decl_defs::ShallowMethod) -> Self {
        Self {
            name: sm.name.into(),
            ty: sm.type_.into(),
            visibility: sm.visibility,
            deprecated: sm.deprecated.map(Into::into),
            attributes: slice(sm.attributes),
            flags: oxidized::method_flags::MethodFlags::from_bits_truncate(sm.flags.bits()),
            package_requirement: sm.package_requirement,
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ShallowProp> for shallow::ShallowProp<R> {
    fn from(sp: o::shallow_decl_defs::ShallowProp) -> Self {
        Self {
            name: sp.name.into(),
            xhp_attr: sp.xhp_attr,
            ty: sp.type_.into(),
            visibility: sp.visibility,
            flags: oxidized::prop_flags::PropFlags::from_bits_truncate(sp.flags.bits()),
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ClassDecl> for shallow::ShallowClass<R> {
    fn from(sc: o::shallow_decl_defs::ClassDecl) -> Self {
        // Destructure to help ensure we convert every field.
        let o::shallow_decl_defs::ClassDecl {
            mode,
            final_,
            abstract_,
            is_xhp,
            internal,
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
            sprops,
            constructor,
            static_methods,
            methods,
            user_attributes,
            enum_type,
            docs_url,
            package,
        } = sc;
        Self {
            mode,
            is_final: final_,
            is_abstract: abstract_,
            is_internal: internal,
            is_xhp,
            has_xhp_keyword,
            kind,
            module: module.map(Into::into),
            name: name.into(),
            tparams: slice(tparams),
            extends: slice(extends),
            uses: slice(uses),
            xhp_attr_uses: slice(xhp_attr_uses),
            xhp_enum_values: (xhp_enum_values.into_iter())
                .map(|(k, v)| (k.into(), slice(v)))
                .collect(),
            xhp_marked_empty,
            req_extends: slice(req_extends),
            req_implements: slice(req_implements),
            req_constraints: slice(req_constraints),
            implements: slice(implements),
            support_dynamic_type,
            consts: slice(consts),
            typeconsts: slice(typeconsts),
            props: slice(props),
            static_props: slice(sprops),
            constructor: constructor.map(Into::into),
            static_methods: slice(static_methods),
            methods: slice(methods),
            user_attributes: slice(user_attributes),
            enum_type: enum_type.map(Into::into),
            docs_url,
            package,
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::FunDecl> for shallow::FunDecl<R> {
    fn from(sf: o::shallow_decl_defs::FunDecl) -> Self {
        Self {
            pos: sf.pos.into(),
            ty: sf.type_.into(),
            deprecated: sf.deprecated.map(Into::into),
            module: sf.module.map(Into::into),
            internal: sf.internal,
            php_std_lib: sf.php_std_lib,
            support_dynamic_type: sf.support_dynamic_type,
            no_auto_dynamic: sf.no_auto_dynamic,
            no_auto_likes: sf.no_auto_likes,
            gated_by_feature_flag: sf.gated_by_feature_flag,
            package: sf.package,
            package_requirement: sf.package_requirement,
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::TypedefDecl> for shallow::TypedefDecl<R> {
    fn from(x: o::shallow_decl_defs::TypedefDecl) -> Self {
        Self {
            module: x.module.map(Into::into),
            pos: x.pos.into(),
            tparams: slice(x.tparams),
            as_constraint: x.as_constraint.map(Into::into),
            super_constraint: x.super_constraint.map(Into::into),
            type_assignment: (x.type_assignment).into(),
            is_ctx: x.is_ctx,
            attributes: slice(x.attributes),
            internal: x.internal,
            docs_url: x.docs_url,
            package: x.package,
        }
    }
}

impl<R: Reason> From<o::decl_defs::TypedefTypeAssignment> for TypedefTypeAssignment<R> {
    fn from(x: o::decl_defs::TypedefTypeAssignment) -> Self {
        use o::decl_defs::TypedefTypeAssignment as O;
        match x {
            O::SimpleTypeDef(vis, ty) => TypedefTypeAssignment::SimpleTypeDef(vis, ty.into()),
            O::CaseType(variant, variants) => {
                TypedefTypeAssignment::CaseType(variant.into(), slice(variants))
            }
        }
    }
}

impl<R: Reason> From<o::decl_defs::TypedefCaseTypeVariant> for TypedefCaseTypeVariant<R> {
    fn from(
        o::decl_defs::TypedefCaseTypeVariant(ty, wcs): o::decl_defs::TypedefCaseTypeVariant,
    ) -> Self {
        TypedefCaseTypeVariant {
            hint: ty.into(),
            where_constraints: slice(wcs),
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ConstDecl> for shallow::ConstDecl<R> {
    fn from(x: o::shallow_decl_defs::ConstDecl) -> Self {
        Self {
            pos: x.pos.into(),
            ty: x.type_.into(),
            value: x.value,
            package: x.package,
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::ModuleDefType> for shallow::ModuleDecl<R> {
    fn from(x: o::shallow_decl_defs::ModuleDefType) -> Self {
        Self {
            pos: x.mdt_pos.into(),
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::Decl> for shallow::Decl<R> {
    fn from(decl: o::shallow_decl_defs::Decl) -> Self {
        use o::shallow_decl_defs::Decl as O;
        match decl {
            O::Class(x) => Self::Class(x.into()),
            O::Fun(x) => Self::Fun(x.into()),
            O::Typedef(x) => Self::Typedef(x.into()),
            O::Const(x) => Self::Const(x.into()),
            O::Module(x) => Self::Module(x.into()),
        }
    }
}

impl<R: Reason> From<(String, o::shallow_decl_defs::Decl)> for shallow::NamedDecl<R> {
    fn from(decl: (String, o::shallow_decl_defs::Decl)) -> Self {
        use o::shallow_decl_defs::Decl as O;
        match decl {
            (name, O::Class(x)) => Self::Class(name.into(), x.into()),
            (name, O::Fun(x)) => Self::Fun(name.into(), x.into()),
            (name, O::Typedef(x)) => Self::Typedef(name.into(), x.into()),
            (name, O::Const(x)) => Self::Const(name.into(), x.into()),
            (name, O::Module(x)) => Self::Module(name.into(), x.into()),
        }
    }
}

impl From<o::decl_defs::Element> for folded::FoldedElement {
    fn from(x: o::decl_defs::Element) -> Self {
        Self {
            flags: x.flags,
            origin: x.origin.into(),
            visibility: x.visibility.into(),
            deprecated: x.deprecated.map(Into::into),
            sealed_allowlist: x.sealed_allowlist.map(map_k),
            overlapping_tparams: x.overlapping_tparams.map(map_k),
            package_requirement: x.package_requirement,
        }
    }
}

impl<R: Reason> From<o::decl_defs::SubstContext> for folded::SubstContext<R> {
    fn from(x: o::decl_defs::SubstContext) -> Self {
        Self {
            subst: folded::Subst(map_kv(x.subst)),
            class_context: x.class_context.into(),
            from_req_extends: x.from_req_extends,
        }
    }
}

impl<R: Reason> From<o::typing_defs::TypeconstType> for folded::TypeConst<R> {
    fn from(x: o::typing_defs::TypeconstType) -> Self {
        Self {
            is_synthesized: x.synthesized,
            name: x.name.into(),
            kind: x.kind.into(),
            origin: x.origin.into(),
            enforceable: x.enforceable.into(),
            reifiable: x.reifiable.map(Into::into),
            is_concretized: x.concretized,
            is_ctx: x.is_ctx,
        }
    }
}

impl<R: Reason> From<o::typing_defs::ClassConst> for folded::ClassConst<R> {
    fn from(x: o::typing_defs::ClassConst) -> Self {
        Self {
            is_synthesized: x.synthesized,
            kind: x.abstract_,
            pos: x.pos.into(),
            ty: x.type_.into(),
            origin: x.origin.into(),
            refs: slice(x.refs),
            is_shape_key: x.is_shape_key,
            value: x.value.into(),
        }
    }
}

impl<R: Reason> From<o::decl_defs::Requirement> for folded::Requirement<R> {
    fn from(req: o::decl_defs::Requirement) -> Self {
        Self {
            pos: req.0.into(),
            ty: req.1.into(),
        }
    }
}

impl<R: Reason> From<o::shallow_decl_defs::DeclConstraintRequirement>
    for DeclConstraintRequirement<R>
{
    fn from(dcr: o::shallow_decl_defs::DeclConstraintRequirement) -> Self {
        use o::shallow_decl_defs::DeclConstraintRequirement as O;
        match dcr {
            O::DCREqual(ty) => DeclConstraintRequirement::DCREqual(ty.into()),
            O::DCRSubtype(ty) => DeclConstraintRequirement::DCRSubtype(ty.into()),
        }
    }
}

impl<R: Reason> From<o::decl_defs::ConstraintRequirement> for folded::ConstraintRequirement<R> {
    fn from(cr: o::decl_defs::ConstraintRequirement) -> Self {
        use o::decl_defs::ConstraintRequirement as O;
        match cr {
            O::CREqual(r) => folded::ConstraintRequirement::CREqual(r.into()),
            O::CRSubtype(r) => folded::ConstraintRequirement::CRSubtype(r.into()),
        }
    }
}

impl From<(Option<o::decl_defs::Element>, ty::ConsistentKind)> for folded::Constructor {
    fn from(construct: (Option<o::decl_defs::Element>, ty::ConsistentKind)) -> Self {
        Self::new(construct.0.map(Into::into), construct.1)
    }
}

impl<P> From<o::decl_defs::DeclError> for crate::decl_error::DeclError<P>
where
    P: From<o::pos::Pos>,
{
    fn from(decl_error: o::decl_defs::DeclError) -> Self {
        use o::decl_defs::DeclError as O;
        match decl_error {
            O::WrongExtendKind {
                pos,
                kind,
                name,
                parent_pos,
                parent_kind,
                parent_name,
            } => Self::WrongExtendKind {
                pos: pos.into(),
                kind,
                name: name.into(),
                parent_pos: parent_pos.into(),
                parent_kind,
                parent_name: parent_name.into(),
            },
            O::WrongUseKind {
                pos,
                name,
                parent_pos,
                parent_name,
            } => Self::WrongUseKind {
                pos: pos.into(),
                name: name.into(),
                parent_pos: parent_pos.into(),
                parent_name: parent_name.into(),
            },
            O::CyclicClassDef { pos, stack } => Self::CyclicClassDef(pos.into(), map_k(stack)),
        }
    }
}

impl<R: Reason> From<o::decl_defs::DeclClassType> for folded::FoldedClass<R> {
    fn from(cls: o::decl_defs::DeclClassType) -> Self {
        // Destructure to help ensure we convert every field. A couple fields
        // are ignored because they're redundant with other fields (and
        // `folded::FoldedClass` just omits the redundant fields).
        let o::decl_defs::DeclClassType {
            name,
            pos,
            kind,
            abstract_: _, // `Self::is_abstract()` just reads the `kind` field
            final_,
            const_,
            internal,
            is_xhp,
            has_xhp_keyword,
            support_dynamic_type,
            module,
            is_module_level_trait,
            tparams,
            substs,
            ancestors,
            props,
            sprops,
            methods,
            smethods,
            consts,
            typeconsts,
            xhp_enum_values,
            xhp_marked_empty,
            construct,
            need_init: _, // `Self::has_concrete_constructor()` reads the `constructor` field
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
            tests_bypass_visibility,
            allow_unchecked_enum_values,
        } = cls;
        Self {
            name: name.into(),
            pos: pos.into(),
            kind,
            is_final: final_,
            is_const: const_,
            is_internal: internal,
            tests_bypass_visibility,
            allow_unchecked_enum_values,
            is_xhp,
            has_xhp_keyword,
            support_dynamic_type,
            enum_type: enum_type.map(Into::into),
            module: module.map(Into::into),
            is_module_level_trait,
            tparams: slice(tparams),
            substs: map_kv(substs),
            ancestors: map_kv(ancestors),
            props: map_kv(props),
            static_props: map_kv(sprops),
            methods: map_kv(methods),
            static_methods: map_kv(smethods),
            constructor: construct.into(),
            consts: map_kv(consts),
            type_consts: map_kv(typeconsts),
            xhp_enum_values: xhp_enum_values
                .into_iter()
                .map(|(k, v)| (k.into(), slice(v)))
                .collect(),
            xhp_marked_empty,
            extends: map_k(extends),
            xhp_attr_deps: map_k(xhp_attr_deps),
            req_ancestors: slice(req_ancestors),
            req_ancestors_extends: map_k(req_ancestors_extends),
            req_constraints_ancestors: slice(req_constraints_ancestors),
            sealed_whitelist: sealed_whitelist.map(map_k),
            deferred_init_members: map_k(deferred_init_members),
            decl_errors: slice(decl_errors),
            docs_url,
            allow_multiple_instantiations,
            package,
        }
    }
}
