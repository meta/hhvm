// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<5bb7336428d106c13bbf0c9f59c0d26e>>
//
// To regenerate this file, run:
//   buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/src:oxidized_regen

use eq_modulo_pos::EqModuloPos;
use no_pos_hash::NoPosHash;
use ocamlrep::FromOcamlRep;
use ocamlrep::FromOcamlRepIn;
use ocamlrep::ToOcamlRep;
use serde::Deserialize;
use serde::Serialize;
pub use typing_defs_core::*;
pub use typing_defs_flags::*;

#[allow(unused_imports)]
use crate::*;

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
#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum ClassConstFrom {
    #[rust_to_ocaml(name = "Self")]
    Self_,
    From(String),
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
#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C)]
pub struct ClassConstRef(pub ClassConstFrom, pub String);

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (show, eq)")]
#[rust_to_ocaml(prefix = "cd_")]
#[repr(C)]
pub struct ConstDecl {
    pub pos: pos_or_decl::PosOrDecl,
    pub type_: Ty,
    pub value: Option<String>,
    pub package: Option<aast_defs::PackageMembership>,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum PackageRequirement {
    RPRequire(PosString),
    RPSoft(PosString),
    RPNormal,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[rust_to_ocaml(prefix = "ce_")]
#[repr(C)]
pub struct ClassElt {
    pub visibility: CeVisibility,
    pub type_: lazy::Lazy<Ty>,
    /// identifies the class from which this elt originates
    pub origin: String,
    pub deprecated: Option<String>,
    /// pos of the type of the elt
    pub pos: lazy::Lazy<pos_or_decl::PosOrDecl>,
    pub flags: typing_defs_flags::class_elt::ClassElt,
    pub sealed_allowlist: Option<s_set::SSet>,
    pub overlapping_tparams: Option<s_set::SSet>,
    pub package_requirement: Option<PackageRequirement>,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (show, eq)")]
#[rust_to_ocaml(prefix = "fe_")]
#[repr(C)]
pub struct FunElt {
    pub deprecated: Option<String>,
    pub module: Option<ast_defs::Id>,
    pub package: Option<aast_defs::PackageMembership>,
    /// Top-level functions have limited visibilities
    pub internal: bool,
    pub type_: Ty,
    pub pos: pos_or_decl::PosOrDecl,
    pub php_std_lib: bool,
    pub support_dynamic_type: bool,
    pub no_auto_dynamic: bool,
    pub no_auto_likes: bool,
    pub gated_by_feature_flag: Option<String>,
    pub package_requirement: PackageRequirement,
}

#[derive(
    Clone,
    Copy,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    FromOcamlRepIn,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum ClassConstKind {
    CCAbstract(bool),
    CCConcrete,
}

/// `CVInt` holds values that fit
/// OCaml's 63-bit `int`; literals outside that range (including `i64::MIN`)
/// use `CVLargeInt`, which stores the canonical decimal as a string so it
/// marshals without truncation and every spelling of one value compares equal.
/// `CVNameof` and `CVClassPointer` are kept distinct so `nameof C` (a
/// string) and `C::class` (a class pointer) never coincide. `CVLabel` means
/// the value is a string equal to the member's own name (`FOO = 'FOO'`),
/// recorded without storing the string.
/// `CVAbsent` represents an unrecorded or unrepresentable value inline, so
/// the recorded value needs no `option` wrapper.
#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, ord, show)")]
#[rust_to_ocaml(attr = r#"warning "-37""#)]
#[repr(C, u8)]
pub enum ConstValue {
    CVInt(isize),
    CVLargeInt(String),
    CVString(String),
    /// Class name with legacy XHP mangling already applied.
    CVNameof(String),
    CVClassPointer(String),
    CVLabel,
    CVAbsent,
    CVConstAccess(String, String),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[rust_to_ocaml(prefix = "cc_")]
#[repr(C)]
pub struct ClassConst {
    pub synthesized: bool,
    pub abstract_: ClassConstKind,
    pub pos: pos_or_decl::PosOrDecl,
    pub type_: Ty,
    /// identifies the class from which this const originates
    pub origin: String,
    /// references to the constants used in the initializer
    pub refs: Vec<ClassConstRef>,
    /// Whether the constant is explicitly or intrinsically permitted as a
    /// shape key.
    pub is_shape_key: bool,
    /// The recorded value for a `__ShapeKey` constant or enum member;
    /// `CVAbsent` means the value was not recorded or cannot be represented.
    pub value: ConstValue,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[repr(C)]
pub struct ModuleDefType {
    pub mdt_pos: pos_or_decl::PosOrDecl,
}

/// The position is that of the hint in the `use` / `implements` AST node
/// that causes a class to have this requirement applied to it. E.g.
///
/// ```
/// class Foo {}
///
/// interface Bar {
///   require extends Foo; <- position of the decl_phase ty
/// }
///
/// class Baz extends Foo implements Bar { <- position of the `implements`
/// }
/// ```
#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C)]
pub struct Requirement(pub pos_or_decl::PosOrDecl, pub Ty);

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum ConstraintRequirement {
    #[rust_to_ocaml(name = "CR_Equal")]
    CREqual(Requirement),
    #[rust_to_ocaml(name = "CR_Subtype")]
    CRSubtype(Requirement),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[rust_to_ocaml(prefix = "atc_")]
#[repr(C)]
pub struct AbstractTypeconst {
    pub as_constraint: Option<Ty>,
    pub super_constraint: Option<Ty>,
    pub default: Option<Ty>,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C)]
pub struct ConcreteTypeconst {
    pub tc_type: Ty,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[rust_to_ocaml(prefix = "patc_")]
#[repr(C)]
pub struct PartiallyAbstractTypeconst {
    pub constraint: Ty,
    pub type_: Ty,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum Typeconst {
    TCAbstract(AbstractTypeconst),
    TCConcrete(ConcreteTypeconst),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[rust_to_ocaml(prefix = "ttc_")]
#[repr(C)]
pub struct TypeconstType {
    pub synthesized: bool,
    pub name: PosId,
    pub kind: Typeconst,
    pub origin: String,
    /// If the typeconst had the <<__Enforceable>> attribute on its
    /// declaration, this will be [(position_of_declaration, true)].
    ///
    /// In legacy decl, the second element of the tuple will also be true if
    /// the typeconst overrides some parent typeconst which had the
    /// <<__Enforceable>> attribute. In that case, the position will point to
    /// the declaration of the parent typeconst.
    ///
    /// In shallow decl, this is not the case--there is no overriding behavior
    /// modeled here, and the second element will only be true when the
    /// declaration of this typeconst had the attribute.
    ///
    /// When the second element of the tuple is false, the position will be
    /// [Pos_or_decl.none].
    ///
    /// To manage the difference between legacy and shallow decl, use
    /// [Folded_class.get_typeconst_enforceability] rather than
    /// accessing this field directly.
    pub enforceable: (pos_or_decl::PosOrDecl, bool),
    pub reifiable: Option<pos_or_decl::PosOrDecl>,
    pub concretized: bool,
    pub is_ctx: bool,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[rust_to_ocaml(prefix = "te_")]
#[repr(C)]
pub struct EnumType {
    pub base: Ty,
    pub constraint: Option<Ty>,
    pub includes: Vec<Ty>,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C)]
pub struct TypedefCaseTypeVariant(pub Ty, pub Vec<WhereConstraint>);

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum TypedefTypeAssignment {
    SimpleTypeDef(ast_defs::TypedefVisibility, Ty),
    CaseType(TypedefCaseTypeVariant, Vec<TypedefCaseTypeVariant>),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[rust_to_ocaml(prefix = "td_")]
#[repr(C)]
pub struct TypedefType {
    pub module: Option<ast_defs::Id>,
    pub pos: pos_or_decl::PosOrDecl,
    pub tparams: Vec<Tparam>,
    pub as_constraint: Option<Ty>,
    pub super_constraint: Option<Ty>,
    pub type_assignment: TypedefTypeAssignment,
    pub is_ctx: bool,
    pub attributes: Vec<UserAttribute>,
    pub internal: bool,
    pub docs_url: Option<String>,
    pub package: Option<aast_defs::PackageMembership>,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show")]
#[repr(C, u8)]
pub enum DeserializationError {
    /// The type was valid, but some component thereof was a decl_ty when we
    /// expected a locl_phase ty, or vice versa.
    #[rust_to_ocaml(name = "Wrong_phase")]
    WrongPhase(String),
    /// The specific type or some component thereof is not one that we support
    /// deserializing, usually because not enough information was serialized to be
    /// able to deserialize it again.
    #[rust_to_ocaml(name = "Not_supported")]
    NotSupported(String),
    /// The input JSON was invalid for some reason.
    #[rust_to_ocaml(name = "Deserialization_error")]
    DeserializationError(String),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving show { with_path = false }")]
#[repr(C, u8)]
pub enum VisibilityBehavior {
    #[rust_to_ocaml(name = "Always_expand_newtype")]
    AlwaysExpandNewtype,
    #[rust_to_ocaml(name = "Expand_visible_newtype_only")]
    ExpandVisibleNewtypeOnly,
    #[rust_to_ocaml(name = "Never_expand_newtype")]
    NeverExpandNewtype,
    #[rust_to_ocaml(name = "Resolve_type_structure")]
    ResolveTypeStructure(Option<String>),
}
