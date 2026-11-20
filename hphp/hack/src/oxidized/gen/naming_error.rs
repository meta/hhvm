// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<182fb94a807e38732ccb142115621366>>
//
// To regenerate this file, run:
//   buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/src:oxidized_regen

use arena_trait::TrivialDrop;
use eq_modulo_pos::EqModuloPos;
use no_pos_hash::NoPosHash;
use ocamlrep::FromOcamlRep;
use ocamlrep::FromOcamlRepIn;
use ocamlrep::ToOcamlRep;
use serde::Deserialize;
use serde::Serialize;

#[allow(unused_imports)]
use crate::*;

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
#[repr(u8)]
pub enum Visibility {
    Vprivate,
    Vpublic,
    Vinternal,
    Vprotected,
    #[rust_to_ocaml(name = "Vprotected_internal")]
    VprotectedInternal,
}
impl TrivialDrop for Visibility {}
arena_deserializer::impl_deserialize_in_arena!(Visibility);

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
#[repr(u8)]
pub enum ReturnOnlyHint {
    Hvoid,
    Hnoreturn,
}
impl TrivialDrop for ReturnOnlyHint {}
arena_deserializer::impl_deserialize_in_arena!(ReturnOnlyHint);

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
#[repr(u8)]
pub enum UnsupportedFeature {
    #[rust_to_ocaml(name = "Ft_where_constraints")]
    FtWhereConstraints,
    #[rust_to_ocaml(name = "Ft_constraints")]
    FtConstraints,
    #[rust_to_ocaml(name = "Ft_reification")]
    FtReification,
    #[rust_to_ocaml(name = "Ft_user_attrs")]
    FtUserAttrs,
    #[rust_to_ocaml(name = "Ft_variance")]
    FtVariance,
}
impl TrivialDrop for UnsupportedFeature {}
arena_deserializer::impl_deserialize_in_arena!(UnsupportedFeature);

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
#[repr(C, u8)]
pub enum NamingError {
    #[rust_to_ocaml(name = "Unexpected_arrow")]
    UnexpectedArrow { pos: pos::Pos, cname: String },
    #[rust_to_ocaml(name = "Missing_arrow")]
    MissingArrow { pos: pos::Pos, cname: String },
    #[rust_to_ocaml(name = "Disallowed_xhp_type")]
    DisallowedXhpType { pos: pos::Pos, ty_name: String },
    #[rust_to_ocaml(name = "Name_is_reserved")]
    NameIsReserved { pos: pos::Pos, name: String },
    #[rust_to_ocaml(name = "Dollardollar_unused")]
    DollardollarUnused(pos::Pos),
    #[rust_to_ocaml(name = "Method_name_already_bound")]
    MethodNameAlreadyBound { pos: pos::Pos, meth_name: String },
    #[rust_to_ocaml(name = "Error_name_already_bound")]
    ErrorNameAlreadyBound {
        pos: pos::Pos,
        name: String,
        prev_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Unbound_name")]
    UnboundName {
        pos: pos::Pos,
        name: String,
        kind: name_context::NameContext,
    },
    #[rust_to_ocaml(name = "Invalid_fun_pointer")]
    InvalidFunPointer { pos: pos::Pos, name: String },
    Undefined {
        pos: pos::Pos,
        var_name: String,
        did_you_mean: Option<(String, pos::Pos)>,
    },
    #[rust_to_ocaml(name = "Undefined_in_expr_tree")]
    UndefinedInExprTree {
        pos: pos::Pos,
        var_name: String,
        dsl: Option<String>,
        did_you_mean: Option<(String, pos::Pos)>,
    },
    #[rust_to_ocaml(name = "This_reserved")]
    ThisReserved(pos::Pos),
    #[rust_to_ocaml(name = "Start_with_T")]
    StartWithT(pos::Pos),
    #[rust_to_ocaml(name = "Already_bound")]
    AlreadyBound { pos: pos::Pos, name: String },
    #[rust_to_ocaml(name = "Unexpected_typedef")]
    UnexpectedTypedef {
        pos: pos::Pos,
        decl_pos: pos::Pos,
        expected_kind: name_context::NameContext,
    },
    #[rust_to_ocaml(name = "Field_name_already_bound")]
    FieldNameAlreadyBound(pos::Pos),
    #[rust_to_ocaml(name = "Named_variadic_type_disallowed")]
    NamedVariadicTypeDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Variadic_named_parameter_disallowed")]
    VariadicNamedParameterDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Trait_self_shape_key")]
    TraitSelfShapeKey { pos: pos::Pos, key_name: String },
    #[rust_to_ocaml(name = "Primitive_top_level")]
    PrimitiveTopLevel(pos::Pos),
    #[rust_to_ocaml(name = "Primitive_invalid_alias")]
    PrimitiveInvalidAlias {
        pos: pos::Pos,
        ty_name_used: String,
        ty_name_canon: String,
    },
    #[rust_to_ocaml(name = "Dynamic_new_in_strict_mode")]
    DynamicNewInStrictMode(pos::Pos),
    #[rust_to_ocaml(name = "Invalid_type_access_root")]
    InvalidTypeAccessRoot { pos: pos::Pos, id: Option<String> },
    #[rust_to_ocaml(name = "Invalid_type_access_in_where")]
    InvalidTypeAccessInWhere(pos::Pos),
    #[rust_to_ocaml(name = "Duplicate_user_attribute")]
    DuplicateUserAttribute {
        pos: pos::Pos,
        attr_name: String,
        prev_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Invalid_memoize_label")]
    InvalidMemoizeLabel { pos: pos::Pos, attr_name: String },
    #[rust_to_ocaml(name = "Unbound_attribute_name")]
    UnboundAttributeName {
        pos: pos::Pos,
        attr_name: String,
        closest_attr_name: Option<String>,
    },
    #[rust_to_ocaml(name = "This_no_argument")]
    ThisNoArgument(pos::Pos),
    #[rust_to_ocaml(name = "Object_cast")]
    ObjectCast(pos::Pos),
    #[rust_to_ocaml(name = "This_hint_outside_class")]
    ThisHintOutsideClass(pos::Pos),
    #[rust_to_ocaml(name = "Parent_outside_class")]
    ParentOutsideClass(pos::Pos),
    #[rust_to_ocaml(name = "Self_outside_class")]
    SelfOutsideClass(pos::Pos),
    #[rust_to_ocaml(name = "Static_outside_class")]
    StaticOutsideClass(pos::Pos),
    #[rust_to_ocaml(name = "This_type_forbidden")]
    ThisTypeForbidden {
        pos: pos::Pos,
        in_extends: bool,
        in_req_extends: bool,
    },
    #[rust_to_ocaml(name = "Nonstatic_property_with_lsb")]
    NonstaticPropertyWithLsb(pos::Pos),
    #[rust_to_ocaml(name = "Classname_param")]
    ClassnameParam(pos::Pos),
    #[rust_to_ocaml(name = "Tparam_applied_to_type")]
    TparamAppliedToType { pos: pos::Pos, tparam_name: String },
    #[rust_to_ocaml(name = "Shadowed_tparam")]
    ShadowedTparam {
        pos: pos::Pos,
        tparam_name: String,
        prev_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Missing_typehint")]
    MissingTypehint(pos::Pos),
    #[rust_to_ocaml(name = "Expected_variable")]
    ExpectedVariable(pos::Pos),
    #[rust_to_ocaml(name = "Too_many_arguments")]
    TooManyArguments(pos::Pos),
    #[rust_to_ocaml(name = "Too_few_arguments")]
    TooFewArguments(pos::Pos),
    #[rust_to_ocaml(name = "Expected_collection")]
    ExpectedCollection { pos: pos::Pos, cname: String },
    #[rust_to_ocaml(name = "Illegal_CLASS")]
    IllegalCLASS(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_TRAIT")]
    IllegalTRAIT(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_member_variable_class")]
    IllegalMemberVariableClass(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_meth_caller")]
    IllegalMethCaller(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_class_meth")]
    IllegalClassMeth(pos::Pos),
    #[rust_to_ocaml(name = "Class_meth_non_final_self")]
    ClassMethNonFinalSelf { pos: pos::Pos, class_name: String },
    #[rust_to_ocaml(name = "Class_meth_non_final_CLASS")]
    ClassMethNonFinalCLASS {
        pos: pos::Pos,
        class_name: String,
        is_trait: bool,
    },
    #[rust_to_ocaml(name = "Const_without_typehint")]
    ConstWithoutTypehint {
        pos: pos::Pos,
        const_name: String,
        ty_name: String,
    },
    #[rust_to_ocaml(name = "Prop_without_typehint")]
    PropWithoutTypehint {
        pos: pos::Pos,
        prop_name: String,
        vis: Visibility,
    },
    #[rust_to_ocaml(name = "Illegal_constant")]
    IllegalConstant(pos::Pos),
    #[rust_to_ocaml(name = "Invalid_require_implements")]
    InvalidRequireImplements(pos::Pos),
    #[rust_to_ocaml(name = "Invalid_require_extends")]
    InvalidRequireExtends(pos::Pos),
    #[rust_to_ocaml(name = "Invalid_require_class")]
    InvalidRequireClass(pos::Pos),
    #[rust_to_ocaml(name = "Invalid_require_constraint")]
    InvalidRequireConstraint(pos::Pos),
    #[rust_to_ocaml(name = "Did_you_mean")]
    DidYouMean {
        pos: pos::Pos,
        name: String,
        suggest_pos: pos::Pos,
        suggest_name: String,
    },
    #[rust_to_ocaml(name = "Using_internal_class")]
    UsingInternalClass { pos: pos::Pos, class_name: String },
    #[rust_to_ocaml(name = "Too_few_type_arguments")]
    TooFewTypeArguments(pos::Pos),
    #[rust_to_ocaml(name = "Dynamic_class_name_in_strict_mode")]
    DynamicClassNameInStrictMode(pos::Pos),
    #[rust_to_ocaml(name = "Xhp_optional_required_attr")]
    XhpOptionalRequiredAttr { pos: pos::Pos, attr_name: String },
    #[rust_to_ocaml(name = "Wildcard_hint_disallowed")]
    WildcardHintDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Wildcard_tparam_disallowed")]
    WildcardTparamDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_use_of_dynamically_callable")]
    IllegalUseOfDynamicallyCallable {
        attr_pos: pos::Pos,
        meth_pos: pos::Pos,
        vis: Visibility,
    },
    #[rust_to_ocaml(name = "Parent_in_function_pointer")]
    ParentInFunctionPointer {
        pos: pos::Pos,
        meth_name: String,
        parent_name: Option<String>,
    },
    #[rust_to_ocaml(name = "Self_in_non_final_function_pointer")]
    SelfInNonFinalFunctionPointer {
        pos: pos::Pos,
        meth_name: String,
        class_name: Option<String>,
    },
    #[rust_to_ocaml(name = "Invalid_wildcard_context")]
    InvalidWildcardContext(pos::Pos),
    #[rust_to_ocaml(name = "Return_only_typehint")]
    ReturnOnlyTypehint { pos: pos::Pos, kind: ReturnOnlyHint },
    #[rust_to_ocaml(name = "Unexpected_type_arguments")]
    UnexpectedTypeArguments(pos::Pos),
    #[rust_to_ocaml(name = "Too_many_type_arguments")]
    TooManyTypeArguments(pos::Pos),
    #[rust_to_ocaml(name = "This_as_lexical_variable")]
    ThisAsLexicalVariable(pos::Pos),
    #[rust_to_ocaml(name = "Explicit_consistent_constructor")]
    ExplicitConsistentConstructor {
        pos: pos::Pos,
        classish_kind: ast_defs::ClassishKind,
    },
    #[rust_to_ocaml(name = "Module_declaration_outside_allowed_files")]
    ModuleDeclarationOutsideAllowedFiles(pos::Pos),
    #[rust_to_ocaml(name = "Internal_module_level_trait")]
    InternalModuleLevelTrait(pos::Pos),
    #[rust_to_ocaml(name = "Dynamic_method_access")]
    DynamicMethodAccess(pos::Pos),
    #[rust_to_ocaml(name = "Deprecated_use")]
    DeprecatedUse { pos: pos::Pos, fn_name: String },
    #[rust_to_ocaml(name = "Unnecessary_attribute")]
    UnnecessaryAttribute {
        pos: pos::Pos,
        attr: String,
        class_pos: pos::Pos,
        class_name: String,
        suggestion: Option<String>,
    },
    #[rust_to_ocaml(name = "Dynamic_hint_disallowed")]
    DynamicHintDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Toplevel_statement")]
    ToplevelStatement(pos::Pos),
    #[rust_to_ocaml(name = "Attribute_outside_allowed_files")]
    AttributeOutsideAllowedFiles(pos::Pos),
    #[rust_to_ocaml(name = "Polymorphic_lambda_missing_return_hint")]
    PolymorphicLambdaMissingReturnHint(pos::Pos),
    #[rust_to_ocaml(prefix = "param_")]
    #[rust_to_ocaml(name = "Polymorphic_lambda_missing_param_hint")]
    PolymorphicLambdaMissingParamHint { pos: pos::Pos, name: String },
}
