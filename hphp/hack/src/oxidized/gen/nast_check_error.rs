// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<24422564e313d7ece0ae4d3ece5b0047>>
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

pub use crate::error_codes::NastCheck as Error_code;
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
pub enum Verb {
    #[rust_to_ocaml(name = "Vreq_implement")]
    VreqImplement,
    Vimplement,
}
impl TrivialDrop for Verb {}
arena_deserializer::impl_deserialize_in_arena!(Verb);

/// The construct that asked whether a package is deployed. Rendered (with
/// styling) by the error constructor.
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
pub enum PackageConstruct {
    #[rust_to_ocaml(name = "Package_expression")]
    PackageExpression,
    #[rust_to_ocaml(name = "Require_package_attribute")]
    RequirePackageAttribute(String),
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
#[repr(C, u8)]
pub enum NastCheckError {
    #[rust_to_ocaml(name = "Repeated_record_field_name")]
    RepeatedRecordFieldName {
        pos: pos::Pos,
        name: String,
        prev_pos: pos_or_decl::PosOrDecl,
    },
    #[rust_to_ocaml(name = "Dynamically_callable_reified")]
    DynamicallyCallableReified(pos::Pos),
    #[rust_to_ocaml(name = "No_construct_parent")]
    NoConstructParent(pos::Pos),
    #[rust_to_ocaml(name = "Nonstatic_method_in_abstract_final_class")]
    NonstaticMethodInAbstractFinalClass(pos::Pos),
    #[rust_to_ocaml(name = "Constructor_required")]
    ConstructorRequired {
        pos: pos::Pos,
        class_name: String,
        prop_names: Vec<String>,
    },
    #[rust_to_ocaml(name = "Not_initialized")]
    NotInitialized {
        pos: pos::Pos,
        class_name: String,
        props: Vec<(pos_or_decl::PosOrDecl, String)>,
    },
    #[rust_to_ocaml(name = "Call_before_init")]
    CallBeforeInit {
        pos: pos::Pos,
        prop_name: String,
    },
    #[rust_to_ocaml(name = "Abstract_with_body")]
    AbstractWithBody(pos::Pos),
    #[rust_to_ocaml(name = "Not_abstract_without_typeconst")]
    NotAbstractWithoutTypeconst(pos::Pos),
    #[rust_to_ocaml(name = "Typeconst_depends_on_external_tparam")]
    TypeconstDependsOnExternalTparam {
        pos: pos::Pos,
        ext_pos: pos::Pos,
        ext_name: String,
    },
    #[rust_to_ocaml(name = "Interface_with_partial_typeconst")]
    InterfaceWithPartialTypeconst(pos::Pos),
    #[rust_to_ocaml(name = "Partially_abstract_typeconst_definition")]
    PartiallyAbstractTypeconstDefinition(pos::Pos),
    #[rust_to_ocaml(name = "Refinement_in_typestruct")]
    RefinementInTypestruct {
        pos: pos::Pos,
        kind: String,
    },
    #[rust_to_ocaml(name = "Multiple_xhp_category")]
    MultipleXhpCategory(pos::Pos),
    #[rust_to_ocaml(name = "Return_in_finally")]
    ReturnInFinally(pos::Pos),
    #[rust_to_ocaml(name = "Toplevel_break")]
    ToplevelBreak(pos::Pos),
    #[rust_to_ocaml(name = "Toplevel_continue")]
    ToplevelContinue(pos::Pos),
    #[rust_to_ocaml(name = "Continue_in_switch")]
    ContinueInSwitch(pos::Pos),
    #[rust_to_ocaml(name = "Await_in_sync_function")]
    AwaitInSyncFunction {
        pos: pos::Pos,
        func_pos: Option<pos::Pos>,
        keyword: String,
    },
    #[rust_to_ocaml(name = "Interface_uses_trait")]
    InterfaceUsesTrait(pos::Pos),
    Magic {
        pos: pos::Pos,
        meth_name: String,
    },
    #[rust_to_ocaml(name = "Non_interface")]
    NonInterface {
        pos: pos::Pos,
        name: String,
        verb: Verb,
    },
    #[rust_to_ocaml(name = "ToString_returns_string")]
    ToStringReturnsString(pos::Pos),
    #[rust_to_ocaml(name = "ToString_visibility")]
    ToStringVisibility(pos::Pos),
    #[rust_to_ocaml(name = "Uses_non_trait")]
    UsesNonTrait {
        pos: pos::Pos,
        name: String,
        kind: String,
    },
    #[rust_to_ocaml(name = "Requires_non_class")]
    RequiresNonClass {
        pos: pos::Pos,
        name: String,
        kind: String,
        is_interface: bool,
        is_req_this_as_check: bool,
    },
    #[rust_to_ocaml(name = "Requires_final_class")]
    RequiresFinalClass {
        pos: pos::Pos,
        name: String,
        is_req_this_as_check: bool,
    },
    #[rust_to_ocaml(name = "Abstract_body")]
    AbstractBody(pos::Pos),
    #[rust_to_ocaml(name = "Interface_with_member_variable")]
    InterfaceWithMemberVariable(pos::Pos),
    #[rust_to_ocaml(name = "Interface_with_static_member_variable")]
    InterfaceWithStaticMemberVariable(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_function_name")]
    IllegalFunctionName {
        pos: pos::Pos,
        name: String,
    },
    #[rust_to_ocaml(name = "Entrypoint_arguments")]
    EntrypointArguments(pos::Pos),
    #[rust_to_ocaml(name = "Entrypoint_generics")]
    EntrypointGenerics(pos::Pos),
    #[rust_to_ocaml(name = "Variadic_memoize")]
    VariadicMemoize(pos::Pos),
    #[rust_to_ocaml(name = "Abstract_method_memoize")]
    AbstractMethodMemoize(pos::Pos),
    #[rust_to_ocaml(name = "Instance_property_in_abstract_final_class")]
    InstancePropertyInAbstractFinalClass(pos::Pos),
    #[rust_to_ocaml(name = "Inout_params_special")]
    InoutParamsSpecial(pos::Pos),
    #[rust_to_ocaml(name = "Inout_params_memoize")]
    InoutParamsMemoize {
        pos: pos::Pos,
        param_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Inout_in_transformed_pseudofunction")]
    InoutInTransformedPseudofunction {
        pos: pos::Pos,
        fn_name: String,
    },
    #[rust_to_ocaml(name = "Named_in_transformed_pseudofunction")]
    NamedInTransformedPseudofunction {
        pos: pos::Pos,
        fn_name: String,
    },
    #[rust_to_ocaml(name = "Reading_from_append")]
    ReadingFromAppend(pos::Pos),
    #[rust_to_ocaml(name = "List_rvalue")]
    ListRvalue(pos::Pos),
    #[rust_to_ocaml(name = "Placeholder_read")]
    PlaceholderRead(pos::Pos),
    #[rust_to_ocaml(name = "Placeholder_inout_write")]
    PlaceholderInoutWrite {
        pos: pos::Pos,
        param_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Illegal_destructor")]
    IllegalDestructor(pos::Pos),
    #[rust_to_ocaml(name = "Illegal_context")]
    IllegalContext {
        pos: pos::Pos,
        name: String,
    },
    #[rust_to_ocaml(name = "Case_fallthrough")]
    CaseFallthrough {
        switch_pos: pos::Pos,
        case_pos: pos::Pos,
        next_pos: Option<pos::Pos>,
    },
    #[rust_to_ocaml(name = "Default_fallthrough")]
    DefaultFallthrough(pos::Pos),
    #[rust_to_ocaml(name = "Php_lambda_disallowed")]
    PhpLambdaDisallowed(pos::Pos),
    #[rust_to_ocaml(name = "Internal_method_with_invalid_visibility")]
    InternalMethodWithInvalidVisibility {
        pos: pos::Pos,
        vis: ast_defs::Visibility,
    },
    #[rust_to_ocaml(name = "Private_and_final")]
    PrivateAndFinal(pos::Pos),
    #[rust_to_ocaml(name = "Internal_member_inside_public_trait")]
    InternalMemberInsidePublicTrait {
        member_pos: pos::Pos,
        trait_pos: pos::Pos,
        is_method: bool,
    },
    #[rust_to_ocaml(name = "Attribute_conflicting_memoize")]
    AttributeConflictingMemoize {
        pos: pos::Pos,
        second_pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Soft_internal_without_internal")]
    SoftInternalWithoutInternal(pos::Pos),
    #[rust_to_ocaml(name = "Wrong_expression_kind_builtin_attribute")]
    WrongExpressionKindBuiltinAttribute {
        pos: pos::Pos,
        attr_name: String,
        expr_kind: String,
    },
    #[rust_to_ocaml(name = "Attribute_too_many_arguments")]
    AttributeTooManyArguments {
        pos: pos::Pos,
        name: String,
        expected: isize,
    },
    #[rust_to_ocaml(name = "Attribute_too_few_arguments")]
    AttributeTooFewArguments {
        pos: pos::Pos,
        name: String,
        expected: isize,
    },
    #[rust_to_ocaml(name = "Attribute_not_exact_number_of_args")]
    AttributeNotExactNumberOfArgs {
        pos: pos::Pos,
        name: String,
        actual: isize,
        expected: isize,
    },
    #[rust_to_ocaml(name = "Attribute_param_type")]
    AttributeParamType {
        pos: pos::Pos,
        x: String,
    },
    #[rust_to_ocaml(name = "Attribute_no_auto_dynamic")]
    AttributeNoAutoDynamic(pos::Pos),
    #[rust_to_ocaml(name = "Attribute_implemented_by_restriction")]
    AttributeImplementedByRestriction {
        restriction: String,
        pos: pos::Pos,
    },
    #[rust_to_ocaml(name = "Generic_at_runtime")]
    GenericAtRuntime {
        pos: pos::Pos,
        prefix: String,
    },
    #[rust_to_ocaml(name = "Generics_not_allowed")]
    GenericsNotAllowed(pos::Pos),
    #[rust_to_ocaml(name = "Local_variable_modified_and_used")]
    LocalVariableModifiedAndUsed {
        pos: pos::Pos,
        pos_useds: Vec<pos::Pos>,
    },
    #[rust_to_ocaml(name = "Local_variable_modified_twice")]
    LocalVariableModifiedTwice {
        pos: pos::Pos,
        pos_modifieds: Vec<pos::Pos>,
    },
    #[rust_to_ocaml(name = "Assign_during_case")]
    AssignDuringCase(pos::Pos),
    #[rust_to_ocaml(name = "Read_before_write")]
    ReadBeforeWrite {
        pos: pos::Pos,
        member_name: String,
    },
    #[rust_to_ocaml(name = "Lateinit_with_default")]
    LateinitWithDefault(pos::Pos),
    #[rust_to_ocaml(name = "Missing_assign")]
    MissingAssign(pos::Pos),
    #[rust_to_ocaml(name = "Clone_return_type")]
    CloneReturnType(pos::Pos),
    #[rust_to_ocaml(name = "Class_sealed_with_trait")]
    ClassSealedWithTrait {
        pos: pos::Pos,
        class_name: String,
        trait_name: String,
    },
    #[rust_to_ocaml(name = "Require_package_strict_inclusion")]
    RequirePackageStrictInclusion {
        required_pos: pos::Pos,
        required: String,
        def_pos: pos_or_decl::PosOrDecl,
        current: String,
        current_pos: pos::Pos,
        attribute_name: String,
        current_package_assignment_kind: String,
    },
    #[rust_to_ocaml(name = "Package_expression_strict_inclusion")]
    PackageExpressionStrictInclusion {
        pkg_pos: pos::Pos,
        pkg: String,
        def_pos: pos_or_decl::PosOrDecl,
        current: String,
        current_pos: pos::Pos,
        current_package_assignment_kind: String,
    },
    #[rust_to_ocaml(name = "Observation_not_allowed_for_package")]
    ObservationNotAllowedForPackage {
        pos: pos::Pos,
        pkg: String,
        def_pos: pos_or_decl::PosOrDecl,
        construct: PackageConstruct,
    },
    #[rust_to_ocaml(name = "Override_not_allowed_for_package")]
    OverrideNotAllowedForPackage {
        pos: pos::Pos,
        pkg: String,
        def_pos: pos_or_decl::PosOrDecl,
        attr: String,
    },
    #[rust_to_ocaml(name = "Package_override_target_not_included")]
    PackageOverrideTargetNotIncluded {
        override_pos: pos::Pos,
        target: String,
        path_package: String,
        path_package_pos: pos_or_decl::PosOrDecl,
    },
    #[rust_to_ocaml(name = "Redundant_package_override")]
    RedundantPackageOverride {
        pos: pos::Pos,
        pkg: String,
        def_pos: pos_or_decl::PosOrDecl,
    },
}

/// Context for package strict inclusion errors
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
pub enum PackageStrictInclusionContext {
    #[rust_to_ocaml(name = "Ctx_require_package")]
    CtxRequirePackage(String),
    #[rust_to_ocaml(name = "Ctx_package_expression")]
    CtxPackageExpression,
}
