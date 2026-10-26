// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::ops::BitAnd;
use std::ops::BitAndAssign;
use std::ops::BitOr;
use std::ops::Sub;
use std::ops::SubAssign;

#[allow(unreachable_patterns)]
#[cxx::bridge(namespace = "HPHP")]
pub mod ffi {
    // This is not a real definition. Cxx just adds static asserts on each of these enum variants
    // that they match the definition in attr.h.
    #[repr(u32)]
    #[derive(Debug, Copy, Clone, Serialize)]
    pub enum Attr {
        AttrNone = 0x0,
        AttrForbidDynamicProps = 0x1,
        AttrDeepInit = 0x1,
        AttrPublic = 0x2,
        AttrProtected = 0x4,
        AttrPrivate = 0x8,
        AttrEnum = 0x10,
        AttrSystemInitialValue = 0x20,
        AttrNoImplicitNullable = 0x40,
        AttrStatic = 0x10,
        AttrAbstract = 0x20,
        AttrFinal = 0x40,
        AttrInterface = 0x80,
        AttrLSB = 0x80,
        AttrSupportsAsyncEagerReturn = 0x80,
        AttrTrait = 0x100,
        AttrNoInjection = 0x200,
        AttrInitialSatisfiesTC = 0x200,
        AttrNoBadRedeclare = 0x400,
        AttrEagerOnly = 0x400,
        AttrInterceptable = 0x800,
        AttrSealed = 0x800,
        AttrLateInit = 0x800,
        AttrNoExpandTrait = 0x1000,
        AttrSplatParam = 0x1000,
        AttrNoOverride = 0x2000,
        AttrIsReadonly = 0x4000,
        AttrReadonlyThis = 0x4000,
        AttrReadonlyReturn = 0x8000,
        AttrInternal = 0x10000,
        AttrInternalSoft = 0x20000,
        AttrPersistent = 0x40000,
        AttrDynamicallyCallable = 0x80000,
        AttrDynamicallyConstructible = 0x80000,
        AttrBuiltin = 0x100000,
        AttrIsConst = 0x200000,
        AttrDynamicallyReferenced = 0x400000,
        AttrNoReifiedInit = 0x800000,
        AttrVariadicNamedParam = 0x800000,
        AttrIsMethCaller = 0x1000000,
        AttrIsClosureClass = 0x1000000,
        AttrHasClosureCoeffectsProp = 0x2000000,
        AttrHasCoeffectRules = 0x2000000,
        AttrIsFoldable = 0x4000000,
        AttrNoFCallBuiltin = 0x8000000,
        AttrInStrictPackage = 0x8000000,
        AttrVariadicParam = 0x10000000,
        AttrProvenanceSkipFrame = 0x20000000,
        AttrEnumClass = 0x40000000,
        AttrUnusedMaxAttr = 0x80000000,
    }

    #[repr(u32)]
    #[derive(Debug, Copy, Clone, Serialize)]
    enum AttrContext {
        Class = 0x1,
        Func = 0x2,
        Prop = 0x4,
        TraitImport = 0x8,
        Alias = 0x10,
        Parameter = 0x20,
        Constant = 0x40,
        Module = 0x80,
    }

    #[repr(u16)]
    #[derive(Debug, Copy, Clone, Hash, Serialize)]
    enum TypeConstraintFlags {
        NoFlags = 0x0,
        SingleTypeConstraint = 0x1,
        Nullable = 0x2,
        Union = 0x4,
        TypeVar = 0x8,
        Soft = 0x10,
        TypeConstant = 0x20,
        Resolved = 0x40,
        DisplayNullable = 0x100,
        UpperBound = 0x200,
    }

    #[repr(u8)]
    #[derive(Debug, Copy, Clone, Hash, Serialize)]
    enum TypeStructureKind {
        T_void = 0,
        T_int = 1,
        T_bool = 2,
        T_float = 3,
        T_string = 4,
        T_resource = 5,
        T_num = 6,
        T_arraykey = 7,
        T_noreturn = 8,
        T_mixed = 9,
        T_tuple = 10,
        T_fun = 11,
        T_typevar = 13, // corresponds to user OF_GENERIC
        T_shape = 14,

        // These values are only used after resolution in ext_reflection.cpp
        T_class = 15,
        T_interface = 16,
        T_trait = 17,
        T_enum = 18,

        // Hack array types
        T_dict = 19,
        T_vec = 20,
        T_keyset = 21,
        T_vec_or_dict = 22,

        T_nonnull = 23,

        T_darray = 24,
        T_varray = 25,
        T_varray_or_darray = 26,
        T_any_array = 27,

        T_null = 28,
        T_nothing = 29,
        T_dynamic = 30,
        T_union = 31,
        T_recursiveUnion = 32,

        T_class_ptr = 33,
        T_class_or_classname = 34,

        // The following kinds needs class/alias resolution, and
        // are generally not exposed to the users.
        // Unfortunately this is a bit leaky, and a few of these are needed by tooling.
        T_unresolved = 101,
        T_typeaccess = 102,
        T_xhp = 103,
        T_reifiedtype = 104,
    }

    unsafe extern "C++" {
        include!("hphp/hack/src/hackc/hhvm_cxx/hhvm_types/as-base-ffi.h");
        type Attr;
        type AttrContext;
        type TypeConstraintFlags;
        type TypeStructureKind;
        fn attrs_to_string_ffi(ctx: AttrContext, attrs: Attr) -> String;
        fn type_flags_to_string_ffi(flags: TypeConstraintFlags) -> String;
    }
}

pub use ffi::Attr;
pub use ffi::TypeConstraintFlags;
pub use ffi::TypeStructureKind;
use ffi::type_flags_to_string_ffi;

impl Default for Attr {
    fn default() -> Self {
        Attr::AttrNone
    }
}

impl Default for TypeConstraintFlags {
    fn default() -> Self {
        TypeConstraintFlags::NoFlags
    }
}

impl std::fmt::Display for TypeConstraintFlags {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", type_flags_to_string_ffi(*self))
    }
}

impl TypeConstraintFlags {
    pub fn is_empty(&self) -> bool {
        *self == TypeConstraintFlags::NoFlags
    }

    pub fn contains(&self, flag: Self) -> bool {
        (self.repr & flag.repr) != 0
    }
}

impl BitOr for TypeConstraintFlags {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self {
            repr: (self.repr | other.repr),
        }
    }
}

impl BitAnd for TypeConstraintFlags {
    type Output = Self;

    fn bitand(self, other: Self) -> Self {
        TypeConstraintFlags {
            repr: self.repr & other.repr,
        }
    }
}

impl BitAndAssign for TypeConstraintFlags {
    fn bitand_assign(&mut self, rhs: Self) {
        self.repr &= rhs.repr;
    }
}

impl Sub for TypeConstraintFlags {
    type Output = Self;

    fn sub(self, other: Self) -> Self {
        Self {
            repr: self.repr & !other.repr,
        }
    }
}

impl SubAssign for TypeConstraintFlags {
    fn sub_assign(&mut self, other: Self) {
        // For flags subtract just drops the bits.
        self.repr &= !other.repr;
    }
}

impl From<&TypeConstraintFlags> for u16 {
    fn from(r: &TypeConstraintFlags) -> Self {
        r.repr
    }
}

impl From<TypeStructureKind> for i64 {
    fn from(r: TypeStructureKind) -> Self {
        r.repr as i64
    }
}

impl From<u32> for Attr {
    fn from(r: u32) -> Self {
        Self { repr: r }
    }
}

impl From<oxidized::ast_defs::Visibility> for Attr {
    fn from(k: oxidized::ast_defs::Visibility) -> Self {
        use oxidized::ast_defs::Visibility;
        match k {
            Visibility::Private => Self::AttrPrivate,
            Visibility::Public => Self::AttrPublic,
            Visibility::Protected => Self::AttrProtected,
            // TODO(T115356820): Decide whether internal should be mutually
            // exclusive with other visibility modifiers or it should be a
            // modifier on top the others.
            // In order to unblock typechecker, let it be a modifier on top for now.
            Visibility::Internal => Self::AttrInternal | Self::AttrPublic,
            Visibility::ProtectedInternal => Self::AttrInternal | Self::AttrProtected,
        }
    }
}

impl From<&oxidized::ast_defs::Visibility> for Attr {
    fn from(k: &oxidized::ast_defs::Visibility) -> Self {
        use oxidized::ast_defs::Visibility;
        match k {
            Visibility::Private => Self::AttrPrivate,
            Visibility::Public => Self::AttrPublic,
            Visibility::Protected => Self::AttrProtected,
            // TODO(T115356820): Decide whether internal should be mutually
            // exclusive with other visibility modifiers or it should be a
            // modifier on top the others.
            // In order to unblock typechecker, let it be a modifier on top for now.
            Visibility::Internal => Self::AttrInternal | Self::AttrPublic,
            Visibility::ProtectedInternal => Self::AttrInternal | Self::AttrProtected,
        }
    }
}

impl From<Attr> for u32 {
    fn from(attr: Attr) -> Self {
        attr.repr
    }
}

impl Attr {
    pub fn add(&mut self, attr: Attr) {
        *self = *self | attr
    }
    pub fn remove(&mut self, attr: Attr) {
        self.repr &= !attr.repr;
    }
    pub fn contains(&self, attr: Attr) -> bool {
        (self.repr & attr.repr) == attr.repr
    }

    pub fn set(&mut self, attr: Attr, b: bool) {
        if b {
            self.add(attr)
        }
    }

    pub fn clear(&mut self, attr: Attr) {
        self.repr &= !attr.repr;
    }

    pub fn is_closure_class(&self) -> bool {
        (*self & Self::AttrIsClosureClass) != Self::AttrNone
    }
    pub fn is_enum(&self) -> bool {
        (*self & Self::AttrEnum) != Self::AttrNone
    }
    pub fn is_internal(&self) -> bool {
        (*self & Self::AttrInternal) != Self::AttrNone
    }
    pub fn is_public(&self) -> bool {
        (*self & Self::AttrPublic) != Self::AttrNone
    }
    pub fn is_private(&self) -> bool {
        (*self & Self::AttrPrivate) != Self::AttrNone
    }
    pub fn is_protected(&self) -> bool {
        (*self & Self::AttrProtected) != Self::AttrNone
    }
    pub fn is_final(&self) -> bool {
        (*self & Self::AttrFinal) != Self::AttrNone
    }
    pub fn is_sealed(&self) -> bool {
        (*self & Self::AttrSealed) != Self::AttrNone
    }
    pub fn is_abstract(&self) -> bool {
        (*self & Self::AttrAbstract) != Self::AttrNone
    }
    pub fn is_interface(&self) -> bool {
        (*self & Self::AttrInterface) != Self::AttrNone
    }
    pub fn is_trait(&self) -> bool {
        (*self & Self::AttrTrait) != Self::AttrNone
    }
    pub fn is_const(&self) -> bool {
        (*self & Self::AttrIsConst) != Self::AttrNone
    }
    pub fn no_dynamic_props(&self) -> bool {
        (*self & Self::AttrForbidDynamicProps) != Self::AttrNone
    }
    pub fn needs_no_reifiedinit(&self) -> bool {
        (*self & Self::AttrNoReifiedInit) != Self::AttrNone
    }
    pub fn is_late_init(&self) -> bool {
        (*self & Self::AttrLateInit) != Self::AttrNone
    }
    pub fn is_no_bad_redeclare(&self) -> bool {
        (*self & Self::AttrNoBadRedeclare) != Self::AttrNone
    }
    pub fn initial_satisfies_tc(&self) -> bool {
        (*self & Self::AttrInitialSatisfiesTC) != Self::AttrNone
    }
    pub fn no_implicit_null(&self) -> bool {
        (*self & Self::AttrNoImplicitNullable) != Self::AttrNone
    }
    pub fn has_system_initial(&self) -> bool {
        (*self & Self::AttrSystemInitialValue) != Self::AttrNone
    }
    pub fn is_deep_init(&self) -> bool {
        (*self & Self::AttrDeepInit) != Self::AttrNone
    }
    pub fn is_lsb(&self) -> bool {
        (*self & Self::AttrLSB) != Self::AttrNone
    }
    pub fn is_static(&self) -> bool {
        (*self & Self::AttrStatic) != Self::AttrNone
    }
    pub fn is_readonly(&self) -> bool {
        (*self & Self::AttrIsReadonly) != Self::AttrNone
    }
    pub fn is_no_injection(&self) -> bool {
        (*self & Self::AttrNoInjection) != Self::AttrNone
    }
    pub fn is_interceptable(&self) -> bool {
        (*self & Self::AttrInterceptable) != Self::AttrNone
    }
    pub fn is_empty(&self) -> bool {
        *self == Self::AttrNone
    }
}

impl SubAssign for Attr {
    fn sub_assign(&mut self, other: Self) {
        // For flags subtract just drops the bits.
        self.repr &= !other.repr;
    }
}

impl BitOr for Attr {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self {
            repr: self.repr | other.repr,
        }
    }
}

impl BitOr<Attr> for u32 {
    type Output = u32;

    fn bitor(self, other: Attr) -> u32 {
        self | other.repr
    }
}

impl BitAnd for Attr {
    type Output = Self;

    fn bitand(self, other: Self) -> Self {
        Self {
            repr: self.repr & other.repr,
        }
    }
}

impl BitAnd<u32> for Attr {
    type Output = u32;

    fn bitand(self, other: u32) -> u32 {
        self.repr & other
    }
}

impl BitAndAssign for Attr {
    fn bitand_assign(&mut self, rhs: Self) {
        self.repr &= rhs.repr;
    }
}
