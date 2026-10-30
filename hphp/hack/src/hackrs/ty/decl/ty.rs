// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::BTreeMap;
use std::fmt;

use eq_modulo_pos::EqModuloPos;
use hcons::Hc;
use ocamlrep::FromOcamlRep;
use ocamlrep::ToOcamlRep;
use oxidized::aast;
pub use oxidized::aast_defs::PackageMembership;
pub use oxidized::aast_defs::ReifyKind;
pub use oxidized::aast_defs::Tprim as Prim;
use oxidized::ast_defs;
pub use oxidized::ast_defs::Abstraction;
pub use oxidized::ast_defs::ClassishKind;
pub use oxidized::ast_defs::ConstraintKind;
pub use oxidized::ast_defs::Visibility;
pub use oxidized::typing_defs::ClassConstKind;
pub use oxidized::typing_defs::PackageRequirement;
pub use oxidized::typing_defs::Requirement;
pub use oxidized::typing_defs_core::ConsistentKind;
pub use oxidized::typing_defs_core::Enforcement;
pub use oxidized::typing_defs_core::ParamMode;
pub use oxidized::typing_defs_flags;
pub use oxidized::typing_defs_flags::ClassEltFlags;
pub use oxidized::typing_defs_flags::ClassEltFlagsArgs;
pub use oxidized::typing_defs_flags::FunParamFlags;
pub use oxidized::typing_defs_flags::FunTypeFlags;
pub use oxidized::xhp_attribute::Tag;
pub use oxidized::xhp_attribute::XhpAttribute;
use pos::Bytes;
use pos::ModuleName;
use pos::Positioned;
use pos::Symbol;
use pos::ToOxidized;
use pos::TypeConstName;
use pos::TypeName;
use serde::Deserialize;
use serde::Serialize;
use serde::de::DeserializeOwned;
use utils::core::Ident;

use crate::reason;
use crate::reason::Reason;

#[derive(
    Copy,
    Clone,
    Debug,
    Eq,
    EqModuloPos,
    Hash,
    PartialEq,
    Serialize,
    Deserialize
)]
pub enum Exact {
    Exact,
    Nonexact,
}

// c.f. ast_defs::XhpEnumValue
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum XhpEnumValue {
    XEVInt(isize),
    XEVString(Symbol),
}

walkable!(XhpEnumValue => {
    Self::XEVInt(i) => [i],
    Self::XEVString(s) => [s],
});

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum CeVisibility {
    Public,
    Private(TypeName),
    Protected(TypeName),
    Internal(ModuleName),
    ProtectedInternal(TypeName, ModuleName),
}

walkable!(CeVisibility => {
    Self::Public => [],
    Self::Private(t) => [t],
    Self::Protected(t) => [t],
    Self::Internal(m) => [m],
    Self::ProtectedInternal(t, m) => [t, m],
});

// The OCaml type `tshape_field_name` includes positions, but ignores those
// positions in its `ord` implementation. We can't do the same, though: Rust
// hash tables require impls of Hash and Eq to agree, and our Hash impl must
// take positions into account (else hash-consing will produce bad results). We
// could write a custom Ord impl which disagrees with the Eq impl, but it would
// violate the [PartialOrd requirement][] that `a == b` if and only if
// `partial_cmp(a, b) == Some(Equal)`, and the [Ord requirement][] for a strict
// total order.
//
// [PartialOrd requirement]: https://doc.rust-lang.org/std/cmp/trait.PartialOrd.html
// [Ord requirement]: https://doc.rust-lang.org/std/cmp/trait.Ord.html#corollaries
//
// Instead, we omit the positions from these keys, and store the field name's
// position as part of the map's value (in a `ShapeFieldNamePos`).
#[derive(Copy, Clone, Debug, Eq, EqModuloPos, Hash, Ord, PartialEq, PartialOrd)]
#[derive(Serialize, Deserialize)]
pub enum TshapeFieldName {
    TSFregexGroup(Symbol),
    TSFlitStr(Bytes),
    TSFclassConst(TypeName, Symbol),
}

walkable!(TshapeFieldName);

/// The position of a shape field name; e.g., the position of `'a'` in
/// `shape('a' => int)`, or the positions of `Foo` and `X` in
/// `shape(Foo::X => int)`.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, Ord, PartialEq, PartialOrd)]
#[derive(Serialize, Deserialize)]
pub enum ShapeFieldNamePos<P> {
    Simple(P),
    ClassConst(P, P),
}

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum DependentType {
    Texpr(Ident),
}

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum UserAttributeParam {
    Classname(TypeName),
    EnumClassLabel(Symbol),
    String(Bytes),
    Int(Symbol),
}

walkable!(UserAttributeParam);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub struct UserAttribute<P> {
    pub name: Positioned<TypeName, P>,
    pub params: Box<[UserAttributeParam]>,
    pub raw_val: Option<String>,
}

impl<P> UserAttribute<P> {
    pub fn classname_params(&self) -> Vec<TypeName> {
        (self.params.iter())
            .filter_map(|p| match p {
                UserAttributeParam::Classname(cn) => Some(*cn),
                _ => None,
            })
            .collect()
    }
    pub fn string_params(&self) -> Vec<Symbol> {
        (self.params.iter())
            .filter_map(|p| match p {
                UserAttributeParam::String(s) => Some(Symbol::new(s.to_string())),
                _ => None,
            })
            .collect()
    }
}

walkable!(impl<R: Reason> for UserAttribute<R::Pos> => [name, params]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub enum DeclConstraintRequirement<R: Reason> {
    DCREqual(Ty<R>),
    DCRSubtype(Ty<R>),
}

walkable!(impl<R: Reason> for DeclConstraintRequirement<R> => {
    Self::DCREqual(ty) => [ty],
    Self::DCRSubtype(ty) => [ty],
});

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub struct Tparam<R: Reason, TY> {
    pub variance: ast_defs::Variance,
    pub name: Positioned<TypeName, R::Pos>,
    pub constraints: Box<[(ConstraintKind, TY)]>,
    pub reified: ReifyKind,
    pub user_attributes: Box<[UserAttribute<R::Pos>]>,
}

walkable!(impl<R: Reason, TY> for Tparam<R, TY> => [constraints]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub struct WhereConstraint<TY>(pub TY, pub ast_defs::ConstraintKind, pub TY);

walkable!(impl<R: Reason, TY> for WhereConstraint<TY> => [0, 1, 2]);

#[derive(Clone, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct Ty<R: Reason>(R, Hc<Ty_<R>>);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct TypedefCaseTypeVariant<R: Reason> {
    pub hint: Ty<R>,
    pub where_constraints: Box<[WhereConstraint<Ty<R>>]>,
}

walkable!(TypedefCaseTypeVariant<R> => [hint, where_constraints]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub enum TypedefTypeAssignment<R: Reason> {
    SimpleTypeDef(aast::TypedefVisibility, Ty<R>),
    CaseType(TypedefCaseTypeVariant<R>, Box<[TypedefCaseTypeVariant<R>]>),
}

walkable!(impl<R: Reason> for TypedefTypeAssignment<R> => {
    Self::SimpleTypeDef(_, ty) => [ty],
    Self::CaseType(variant, variants) => [variant, variants],
});

walkable!(Ty<R> as visit_decl_ty => [0, 1]);

impl<R: Reason> Ty<R> {
    #[inline]
    pub fn new(reason: R, ty: Ty_<R>) -> Self {
        Self(reason, Hc::new(ty))
    }

    pub fn prim(r: R, prim: Prim) -> Self {
        Self::new(r, Ty_::Tprim(prim))
    }

    pub fn void(r: R) -> Self {
        Self::prim(r, Prim::Tvoid)
    }

    pub fn mixed(r: R) -> Self {
        Self::new(r, Ty_::Tmixed)
    }

    pub fn any(r: R) -> Self {
        Self::new(r, Ty_::Tany)
    }

    pub fn this(r: R) -> Self {
        Self::new(r, Ty_::Tthis)
    }

    pub fn apply(
        reason: R,
        type_name: Positioned<TypeName, R::Pos>,
        tparams: Box<[Ty<R>]>,
    ) -> Self {
        Self::new(reason, Ty_::Tapply(Box::new((type_name, tparams))))
    }

    pub fn class_ptr(reason: R, cls: Ty<R>) -> Self {
        Self::new(reason, Ty_::TclassPtr(cls))
    }

    pub fn generic(reason: R, name: TypeName) -> Self {
        Self::new(reason, Ty_::Tgeneric(name))
    }

    #[inline]
    pub fn access(reason: R, taccess: TaccessType<R, Ty<R>>) -> Self {
        Self::new(reason, Ty_::Taccess(Box::new(taccess)))
    }

    pub fn pos(&self) -> &R::Pos {
        self.0.pos()
    }

    pub fn reason(&self) -> &R {
        &self.0
    }

    pub fn node(&self) -> &Hc<Ty_<R>> {
        &self.1
    }

    pub fn node_ref(&self) -> &Ty_<R> {
        &self.1
    }

    pub fn unwrap_class_type(&self) -> (&R, Positioned<TypeName, R::Pos>, &[Ty<R>]) {
        use Ty_::*;
        let r = self.reason();
        match &**self.node() {
            Tapply(id_and_args) => {
                let (pos_id, args) = &**id_and_args;
                (r, pos_id.clone(), args)
            }
            _ => (r, Positioned::new(r.pos().clone(), TypeName::from("")), &[]),
        }
    }
}

impl<R: Reason> ToOxidized for Ty<R> {
    type Output = oxidized::typing_defs::Ty;

    // For oxidized types, need an owned, heap-allocated Ty_
    fn to_oxidized(self) -> Self::Output {
        oxidized::typing_defs::Ty(
            self.0.to_oxidized(),
            Box::new((*self.1).clone().to_oxidized()),
        )
    }
}

/// A shape may specify whether or not fields are required. For example, consider
/// this typedef:
///
/// ```
/// type ShapeWithOptionalField = shape(?'a' => ?int);
/// ```
///
/// With this definition, the field 'a' may be unprovided in a shape. In this
/// case, the field 'a' would have sf_optional set to true.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub struct ShapeFieldType<R: Reason> {
    pub field_name_pos: ShapeFieldNamePos<R::Pos>,
    pub optional: bool,
    pub ty: Ty<R>,
}

walkable!(ShapeFieldType<R> => [ty]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub struct ShapeTypeSimple<R: Reason>(pub Ty<R>, pub BTreeMap<TshapeFieldName, ShapeFieldType<R>>);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub enum ShapeType<R: Reason> {
    Simple(ShapeTypeSimple<R>),
    Splat(Vec<Ty<R>>),
}

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub struct TupleType<R: Reason>(pub Box<[Ty<R>]>, pub Box<[Ty<R>]>, pub TupleExtra<R>);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub enum TupleExtra<R: Reason> {
    Tsplat(Ty<R>),
    Tvariadic(Ty<R>),
}
walkable!(ShapeTypeSimple<R> => [0, 1]);
walkable!(ShapeType<R> => {
    Self::Simple(inner) => [inner],
    Self::Splat(elems) => [elems],
});

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[serde(bound = "R: Reason")]
pub enum Ty_<R: Reason> {
    /// The late static bound type of a class
    Tthis,
    /// Either an object type or a type alias, ty list are the arguments
    Tapply(Box<(Positioned<TypeName, R::Pos>, Box<[Ty<R>]>)>),
    /// 'With' refinements of the form `_ with { type T as int; type TC = C; }`.
    Trefinement(Box<TrefinementType<Ty<R>>>),
    /// "Any" is the type of a variable with a missing annotation, and "mixed" is
    /// the type of a variable annotated as "mixed". THESE TWO ARE VERY DIFFERENT!
    /// Any unifies with anything, i.e., it is both a supertype and subtype of any
    /// other type. You can do literally anything to it; it's the "trust me" type.
    /// Mixed, on the other hand, is only a supertype of everything. You need to do
    /// a case analysis to figure out what it is (i.e., its elimination form).
    ///
    /// Here's an example to demonstrate:
    ///
    /// ```
    /// function f($x): int {
    ///   return $x + 1;
    /// }
    /// ```
    ///
    /// In that example, $x has type Tany. This unifies with anything, so adding
    /// one to it is allowed, and returning that as int is allowed.
    ///
    /// In contrast, if $x were annotated as mixed, adding one to that would be
    /// a type error -- mixed is not a subtype of int, and you must be a subtype
    /// of int to take part in addition. (The converse is true though -- int is a
    /// subtype of mixed.) A case analysis would need to be done on $x, via
    /// is_int or similar.
    ///
    /// mixed exists only in the decl_phase phase because it is desugared into ?nonnull
    /// during the localization phase.
    Tmixed,
    Twildcard,
    Tlike(Ty<R>),
    Tany,
    Tnonnull,
    /// A dynamic type is a special type which sometimes behaves as if it were a
    /// top type; roughly speaking, where a specific value of a particular type is
    /// expected and that type is dynamic, anything can be given. We call this
    /// behaviour "coercion", in that the types "coerce" to dynamic. In other ways it
    /// behaves like a bottom type; it can be used in any sort of binary expression
    /// or even have object methods called from it. However, it is in fact neither.
    ///
    /// it captures dynamicism within function scope.
    /// See tests in typecheck/dynamic/ for more examples.
    Tdynamic,
    /// Nullable, called "option" in the ML parlance.
    Toption(Ty<R>),
    /// All the primitive types: int, string, void, etc.
    Tprim(aast::Tprim),
    /// A wrapper around fun_type, which contains the full type information for a
    /// function, method, lambda, etc.
    Tfun(Box<FunType<R, Ty<R>>>),
    /// Tuple, with ordered list of the types of the elements of the tuple.
    Ttuple(Box<TupleType<R>>),
    /// Whether all fields of this shape are known, types of each of the
    /// known arms.
    Tshape(Box<ShapeType<R>>),
    /// The type of a generic parameter. The constraints on a generic parameter
    /// are accessed through the lenv.tpenv component of the environment, which
    /// is set up when checking the body of a function or method. See uses of
    /// Typing_phase.add_generic_parameters_and_constraints.
    Tgeneric(TypeName),
    /// Union type.
    /// The values that are members of this type are the union of the values
    /// that are members of the components of the union.
    /// Some examples (writing | for binary union)
    ///   Tunion []  is the "nothing" type, with no values
    ///   Tunion [int;float] is the same as num
    ///   Tunion [null;t] is the same as Toption t
    Tunion(Box<[Ty<R>]>),
    Tintersection(Box<[Ty<R>]>),
    /// Tvec_or_dict (ty1, ty2) => "vec_or_dict<ty1, ty2>"
    TvecOrDict(Box<(Ty<R>, Ty<R>)>),
    Taccess(Box<TaccessType<R, Ty<R>>>),
    /// Class pointer type, matches classname<T>
    TclassPtr(Ty<R>),
}

// We've boxed all variants of Ty_ which are larger than two usizes, so the
// total size should be equal to `[usize; 3]` (one more for the discriminant).
// This is important because all variants use the same amount of memory and are
// passed around by value, so adding a large unboxed variant can cause a large
// regression.
static_assertions::assert_eq_size!(Ty_<reason::NReason>, [usize; 3]);
static_assertions::assert_eq_size!(Ty_<reason::BReason>, [usize; 3]);

impl<R: Reason> hcons::Consable for Ty_<R> {
    #[inline]
    fn conser() -> &'static hcons::Conser<Ty_<R>> {
        R::decl_ty_conser()
    }
}

impl<R: Reason> crate::visitor::Walkable<R> for Ty_<R> {
    fn recurse(&self, v: &mut dyn crate::visitor::Visitor<R>) {
        use Ty_::*;
        match self {
            Tthis | Tmixed | Twildcard | Tany | Tnonnull | Tdynamic | Tprim(_) | Tgeneric(_) => {}
            Tapply(id_and_args) => {
                let (_, args) = &**id_and_args;
                args.accept(v)
            }
            Tlike(ty) | Toption(ty) => ty.accept(v),
            Tfun(ft) => ft.accept(v),
            Tunion(tys) | Tintersection(tys) => tys.accept(v),
            Ttuple(tup) => {
                let TupleType(required, optional, extra) = &**tup;
                required.accept(v);
                optional.accept(v);
                extra.accept(v)
            }
            Tshape(shape) => shape.accept(v),
            TvecOrDict(key_and_val_tys) => {
                let (kty, vty) = &**key_and_val_tys;
                kty.accept(v);
                vty.accept(v)
            }
            Taccess(tt) => tt.accept(v),
            Trefinement(tr) => tr.accept(v),
            TclassPtr(ty) => ty.accept(v),
        }
    }
}

impl<R: Reason> crate::visitor::Walkable<R> for TupleExtra<R> {
    fn recurse(&self, v: &mut dyn crate::visitor::Visitor<R>) {
        match self {
            TupleExtra::Tvariadic(variadic) => variadic.accept(v),
            TupleExtra::Tsplat(splat) => splat.accept(v),
        }
    }
}

/// A Type const access expression of the form <type expr>::C.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub struct TaccessType<R: Reason, TY> {
    /// Type expression to the left of `::`
    pub ty: TY,

    /// Name of type const to the right of `::`
    pub type_const: Positioned<TypeConstName, R::Pos>,
}

walkable!(impl<R: Reason, TY> for TaccessType<R, TY> => [ty]);

/// A decl refinement type of the form 'T with { Refinements }'
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq)]
#[derive(Serialize, Deserialize)]
#[serde(bound = "TY: Serialize + DeserializeOwned")]
pub struct TrefinementType<TY> {
    /// Type expression to the left of `::`
    pub ty: TY,

    /// The refinement
    pub refinement: ClassRefinement<TY>,
}

walkable!(impl<R: Reason> for TrefinementType<Ty<R>> => [ty, refinement]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq)]
#[derive(Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "TY: Serialize + DeserializeOwned")]
pub struct ClassRefinement<TY> {
    pub consts: BTreeMap<TypeConstName, RefinedConst<TY>>,
}

walkable!(impl<R: Reason> for ClassRefinement<Ty<R>> => [consts]);

/// Constant refinements (either `type` or `ctx`)
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq)]
#[derive(Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "TY: Serialize + DeserializeOwned")]
pub struct RefinedConst<TY> {
    pub bound: RefinedConstBound<TY>,
    pub is_ctx: bool,
}

walkable!(impl<R: Reason> for RefinedConst<Ty<R>> => [bound]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq)]
#[derive(Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "TY: Serialize + DeserializeOwned")]
pub enum RefinedConstBound<TY> {
    Exact(TY),
    Loose(RefinedConstBounds<TY>),
}

walkable!(impl<R: Reason, TY> for RefinedConstBound<TY> => {
    Self::Exact(ty) => [ty],
    Self::Loose(bounds) => [bounds],
});

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq)]
#[derive(Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "TY: Serialize + DeserializeOwned")]
pub struct RefinedConstBounds<TY> {
    pub lower: Box<[TY]>,
    pub upper: Box<[TY]>,
}

walkable!(impl<R: Reason, TY> for RefinedConstBounds<TY> => [lower, upper]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub enum Capability<R: Reason, TY> {
    CapDefaults(R::Pos),
    CapTy(TY),
}

walkable!(impl<R: Reason, TY> for Capability<R, TY> => {
    Self::CapDefaults(..) => [],
    Self::CapTy(ty) => [ty],
});

/// Companion to fun_params type, intended to consolidate checking of
/// implicit params for functions.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub struct FunImplicitParams<R: Reason, TY> {
    pub capability: Capability<R, TY>,
}

walkable!(impl<R: Reason, TY> for FunImplicitParams<R, TY> => [capability]);

/// The type of a function AND a method.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub struct FunType<R: Reason, TY> {
    pub tparams: Box<[Tparam<R, TY>]>,
    pub where_constraints: Box<[WhereConstraint<TY>]>,
    pub params: FunParams<R, TY>,
    pub implicit_params: FunImplicitParams<R, TY>,
    /// Carries through the sync/async information from the aast
    pub ret: TY,
    pub flags: typing_defs_flags::FunTypeFlags,
    pub instantiated: bool,
}

walkable!(impl<R: Reason, TY> for FunType<R, TY> => [
    tparams, where_constraints, params, implicit_params, ret
]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason, TY: Serialize + DeserializeOwned")]
pub struct FunParam<R: Reason, TY> {
    pub pos: R::Pos,
    pub name: Option<Symbol>,
    pub ty: TY,
    pub flags: FunParamFlags,
    pub def_value: Option<String>,
}

walkable!(impl<R: Reason, TY> for FunParam<R, TY> => [ty]);

pub type FunParams<R, TY> = Box<[FunParam<R, TY>]>;

/// Origin of Class Constant References:
/// In order to be able to detect cycle definitions like
/// class C {
/// const int A = D::A;
/// }
/// class D {
/// const int A = C::A;
/// }
/// we need to remember which constants were used during initialization.
///
/// Currently the syntax of constants allows direct references to another class
/// like D::A, or self references using self::A.
///
/// class_const_from encodes the origin (class vs self).
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum ClassConstFrom {
    Self_,
    From(TypeName),
}

/// A recorded constant value, used to store enum-member values and
/// class-constant values used as shape keys. `CVInt` holds values that fit
/// OCaml's 63-bit `int`; literals outside that range (including `i64::MIN`) use
/// `CVLargeInt`, which stores the literal's source text verbatim (not
/// canonicalized) so it marshals without truncation.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub enum ConstValue {
    CVInt(isize),
    CVLargeInt(String),
    CVString(String),
    /// Class name with legacy XHP mangling already applied.
    CVNameof(String),
    CVLabel,
    CVAbsent,
    CVConstAccess(String, String),
}

/// Class Constant References:
/// In order to be able to detect cycle definitions like
/// class C {
/// const int A = D::A;
/// }
/// class D {
/// const int A = C::A;
/// }
/// we need to remember which constants were used during initialization.
///
/// Currently the syntax of constants allows direct references to another class
/// like D::A, or self references using self::A.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub struct ClassConstRef(pub ClassConstFrom, pub Symbol);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct ConstDecl<R: Reason> {
    pub pos: R::Pos,
    pub ty: Ty<R>,
    pub value: Option<String>,
    pub package: Option<PackageMembership>,
}

walkable!(ConstDecl<R> => [ty]);

walkable!(PackageMembership);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct FunElt<R: Reason> {
    pub deprecated: Option<Bytes>,
    pub module: Option<Positioned<ModuleName, R::Pos>>,
    pub package: Option<PackageMembership>,
    /// Top-level functions have limited visibilities
    pub internal: bool,
    pub ty: Ty<R>,
    pub pos: R::Pos,
    pub php_std_lib: bool,
    pub support_dynamic_type: bool,
    pub no_auto_dynamic: bool,
    pub no_auto_likes: bool,
    pub gated_by_feature_flag: Option<String>,
    pub package_requirement: PackageRequirement,
}

walkable!(FunElt<R> => [ty]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct AbstractTypeconst<R: Reason> {
    pub as_constraint: Option<Ty<R>>,
    pub super_constraint: Option<Ty<R>>,
    pub default: Option<Ty<R>>,
}

walkable!(AbstractTypeconst<R> => [as_constraint, super_constraint, default]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct ConcreteTypeconst<R: Reason> {
    pub ty: Ty<R>,
}

walkable!(ConcreteTypeconst<R> => [ty]);

#[derive(Clone, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub enum Typeconst<R: Reason> {
    TCAbstract(AbstractTypeconst<R>),
    TCConcrete(ConcreteTypeconst<R>),
}

walkable!(Typeconst<R> => {
    Self::TCAbstract(x) => [x],
    Self::TCConcrete(x) => [x],
});

impl<R: Reason> fmt::Debug for Typeconst<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TCAbstract(x) => x.fmt(f),
            Self::TCConcrete(x) => x.fmt(f),
        }
    }
}

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct EnumType<R: Reason> {
    pub base: Ty<R>,
    pub constraint: Option<Ty<R>>,
    pub includes: Box<[Ty<R>]>,
}

walkable!(EnumType<R> => [base, constraint, includes]);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
#[serde(bound = "R: Reason")]
pub struct TypedefType<R: Reason> {
    pub module: Option<Positioned<ModuleName, R::Pos>>,
    pub pos: R::Pos,
    pub tparams: Box<[Tparam<R, Ty<R>>]>,
    pub as_constraint: Option<Ty<R>>,
    pub super_constraint: Option<Ty<R>>,
    pub type_assignment: TypedefTypeAssignment<R>,
    pub is_ctx: bool,
    pub attributes: Box<[UserAttribute<R::Pos>]>,
    pub internal: bool,
    pub docs_url: Option<String>,
    pub package: Option<PackageMembership>,
}

walkable!(TypedefType<R> => [tparams, as_constraint, super_constraint, type_assignment]);

walkable!(ast_defs::ConstraintKind);

#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
#[derive(ToOcamlRep, FromOcamlRep)]
pub struct ModuleDefType<R: Reason> {
    pub pos: R::Pos,
}

walkable!(ModuleDefType<R> => []);

/// When the option is `Some`, it points to the location of the `__Enforceable`
/// attribute which caused the containing typeconst to be enforceable.
///
/// The newtype allows us to implement ToOxidized and ToOcamlRep in such a way
/// that we produce `(Pos, bool)` tuples, which is how this is represented on
/// the OCaml side.
#[derive(Clone, Debug, Eq, EqModuloPos, Hash, PartialEq, Serialize, Deserialize)]
pub struct Enforceable<P>(pub Option<P>);

impl<P> Enforceable<P> {
    pub fn is_some(&self) -> bool {
        self.0.is_some()
    }
    pub fn is_none(&self) -> bool {
        self.0.is_none()
    }
    pub fn as_ref(&self) -> Option<&P> {
        self.0.as_ref()
    }
}

#[cfg(test)]
mod tests {
    use pos::NPos;

    use super::*;
    use crate::reason::NReason;
    use crate::visitor::Visitor;
    use crate::visitor::Walkable;

    #[derive(Default)]
    struct GenericCollector {
        names: Vec<String>,
    }

    impl Visitor<NReason> for GenericCollector {
        fn object(&mut self) -> &mut dyn Visitor<NReason> {
            self
        }

        fn visit_decl_ty(&mut self, ty: &Ty<NReason>) {
            if let Ty_::Tgeneric(name) = ty.node_ref() {
                self.names.push(name.to_string());
            }
            ty.recurse(self);
        }
    }

    #[test]
    fn visits_shape_tail_and_fields_inside_splats() {
        let tail = Ty::new(
            NReason,
            Ty_::Tunion(
                vec![
                    Ty::generic(NReason, TypeName::from("TailA")),
                    Ty::generic(NReason, TypeName::from("TailB")),
                ]
                .into_boxed_slice(),
            ),
        );
        let fields = BTreeMap::from([(
            TshapeFieldName::TSFlitStr(Bytes::new(b"field")),
            ShapeFieldType {
                field_name_pos: ShapeFieldNamePos::Simple(NPos),
                optional: false,
                ty: Ty::generic(NReason, TypeName::from("Field")),
            },
        )]);
        let shape = Ty::new(
            NReason,
            Ty_::Tshape(Box::new(ShapeType::Simple(ShapeTypeSimple(tail, fields)))),
        );
        let splat = Ty::new(
            NReason,
            Ty_::Tshape(Box::new(ShapeType::Splat(vec![shape.clone()]))),
        );

        for ty in [shape, splat] {
            let mut visitor = GenericCollector::default();
            ty.accept(&mut visitor);
            visitor.names.sort();
            assert_eq!(
                visitor.names,
                vec!["Field".to_owned(), "TailA".to_owned(), "TailB".to_owned()]
            );
        }
    }
}
