(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(*****************************************************************************)
(* Error codes.
 * Each error has a unique number associated with it. The following modules
 * define the error code associated with each kind of error.
 * It is ok to extend the codes with new values, it is NOT OK to change the
 * value of an existing error to a different error code!
 *
 * When removing error codes, you should NOT delete them! Instead, prefer to
 * comment them out as documentation and append DEPRECATED to the name.
 * See below for plenty of examples.
 *)
(*****************************************************************************)

module Parsing = struct
  type t =
    | FixmeFormat [@value 1001]
    | ParsingError [@value 1002]
    (* | UnexpectedEofDEPRECATED [@value 1003] *)
    (* | UnterminatedCommentDEPRECATED [@value 1004] *)
    (* | UnterminatedXhpCommentDEPRECATED [@value 1005] *)
    (* | CallTimePassByReferenceDEPRECATED [@value 1006] *)
    | XhpParsingError [@value 1007]
    | HhIgnoreComment [@value 1008]
    | PackageConfigError [@value 1009]
  (* Add new Parsing codes here! Comment out when deprecating. *)
  [@@deriving enum, show { with_path = false }]

  let err_code = to_enum
end

module Naming = struct
  type t =
    | AddATypehint [@value 2001]
    (* | TypeparamAlokDEPRECATED [@value 2002] *)
    (* | AssertArityDEPRECATED [@value 2003] *)
    | PrimitiveInvalidAlias [@value 2004]
    (* | CyclicConstraintDEPRECATED [@value 2005] *)
    | DidYouMeanNaming [@value 2006]
    (* | DifferentScopeDEPRECATED [@value 2007] *)
    | DisallowedXhpType [@value 2008]
    (* | DoubleInsteadOfFloatDEPRECATED [@value 2009] *)
    (* | DynamicClassDEPRECATED [@value 2010] *)
    (* | LvarInObjGetDEPRECATED [@value 2011] *)
    | ErrorNameAlreadyBound [@value 2012]
    | ExpectedCollection [@value 2013]
    | ExpectedVariable [@value 2014]
    | FdNameAlreadyBound [@value 2015]
    (* | GenArrayRecArityDEPRECATED [@value 2016] *)
    (* | GenArrayVaRecArityDEPRECATED [@value 2017] *)
    (* | GenaArityDEPRECATED [@value 2018] *)
    (* | GenericClassVarDEPRECATED [@value 2019] *)
    (* | GenvaArityDEPRECATED [@value 2020] *)
    | IllegalClass [@value 2021]
    | IllegalClassMeth [@value 2022]
    | IllegalConstant [@value 2023]
    (* | IllegalFunDEPRECATED [@value 2024] *)
    (* | IllegalInstMethDEPRECATED [@value 2025] *)
    | IllegalMethCaller [@value 2026]
    (* | IllegalMethFunDEPRECATED [@value 2027] *)
    (* | IntegerInsteadOfIntDEPRECATED [@value 2028] *)
    | InvalidReqExtends [@value 2029]
    | InvalidReqImplements [@value 2030]
    (* | LocalConstDEPRECATED [@value 2031] *)
    (* | LowercaseThisDEPRECATED [@value 2032] *)
    | MethodNameAlreadyBound [@value 2033]
    | MissingArrow [@value 2034]
    | MissingTypehint [@value 2035]
    | NameAlreadyBound [@value 2036]
    | NamingTooFewArguments [@value 2037]
    | NamingTooManyArguments [@value 2038]
    | PrimitiveToplevel [@value 2039]
    (* | RealInsteadOfFloatDEPRECATED [@value 2040] *)
    | ShadowedTypeParam [@value 2041]
    | StartWith_T [@value 2042]
    | ThisMustBeReturn [@value 2043]
    | ThisNoArgument [@value 2044]
    | ThisHintOutsideClass [@value 2045]
    | ThisReserved [@value 2046]
    | HigherKindedTypesUnsupportedFeature [@value 2047]
    (* | TypedefConstraintDEPRECATED [@value 2048] *)
    | UnboundName [@value 2049]
    | Undefined [@value 2050]
    | UnexpectedArrow [@value 2051]
    | UnexpectedTypedef [@value 2052]
    | UsingInternalClass [@value 2053]
    (* | VoidCastDEPRECATED [@value 2054] *)
    | ObjectCast [@value 2055]
    (* | UnsetCastDEPRECATED [@value 2056] *)
    (* | NullsafePropertyAccessDEPRECATED [@value 2057] *)
    | IllegalTrait [@value 2058]
    (* | ShapeTypehintDEPRECATED [@value 2059] *)
    | DynamicNewInStrictMode [@value 2060]
    | InvalidTypeAccessRoot [@value 2061]
    | DuplicateUserAttribute [@value 2062]
    | ReturnOnlyTypehint [@value 2063]
    | UnexpectedTypeArguments [@value 2064]
    | TooManyTypeArguments [@value 2065]
    | ClassnameParam [@value 2066]
    (* | InvalidInstanceofDEPRECATED [@value 2067] *)
    | NameIsReserved [@value 2068]
    | DollardollarUnused [@value 2069]
    | IllegalMemberVariableClass [@value 2070]
    | TooFewTypeArguments [@value 2071]
    (* | GotoLabelAlreadyDefinedDEPRECATED [@value 2072] *)
    (* | GotoLabelUndefinedDEPRECATED [@value 2073] *)
    (* | GotoLabelDefinedInFinallyDEPRECATED [@value 2074] *)
    (* | GotoInvokedInFinallyDEPRECATED [@value 2075] *)
    (* | DynamicClassPropertyNameInStrictModeDEPRECATED [@value 2076] *)
    | ThisAsLexicalVariable [@value 2077]
    | DynamicClassNameInStrictMode [@value 2078]
    | XhpOptionalRequiredAttr [@value 2079]
    (* | XhpRequiredWithDefaultDEPRECATED [@value 2080] *)
    (* | VariableVariablesDisallowedDEPRECATED [@value 2081] *)
    (* | ArrayTypehintsDisallowedDEPRECATED [@value 2082] *)
    (* | ArrayLiteralsDisallowedDEPRECATED [@value 2083] *)
    | WildcardHintDisallowed [@value 2084]
    (* | AttributeClassNameConflictDEPRECATED [@value 2085] *)
    | MethodNeedsVisibility [@value 2086]
    (* | ReferenceInStrictModeDEPRECATED [@value 2087] *)
    (* | ReferenceInRxDEPRECATED [@value 2088] *)
    (* | DeclareStatementDEPRECATED [@value 2089] *)
    (* | MisplacedRxOfScopeDEPRECATED [@value 2090] *)
    (* | RxOfScopeAndExplicitRxDEPRECATED [@value 2091] *)
    (* | UnsupportedFeatureDEPRECATED [@value 2092] *)
    (* | TraitInterfaceConstructorPromoDEPRECATED [@value 2093] *)
    | NonstaticPropertyWithLSB [@value 2094]
    (* | ReferenceInAnonUseClauseDEPRECATED [@value 2095] *)
    (* | RxMoveInvalidLocationDEPRECATED [@value 2096] *)
    (* | MisplacedMutabilityHintDEPRECATED [@value 2097] *)
    (* | MutabilityHintInNonRxDEPRECATED [@value 2098] *)
    (* | InvalidReturnMutableHintDEPRECATED [@value 2099] *)
    (* | NoTparamsOnTypeConstsDEPRECATED [@value 2100] *)
    (* | PocketUniversesDuplicationDEPRECATED [@value 2101] *)
    (* | UnsupportedTraitUseAsDEPRECATED [@value 2102] *)
    (* | UnsupportedInsteadOfDEPRECATED [@value 2103] *)
    (* | InvalidTraitUseAsVisibilityDEPRECATED [@value 2104] *)
    | InvalidFunPointer [@value 2105]
    | IllegalUseOfDynamicallyCallable [@value 2106]
    (* | PocketUniversesNotInClassDEPRECATED [@value 2107] *)
    (* | PocketUniversesAtomMissingDEPRECATED [@value 2108] *)
    (* | PocketUniversesAtomUnknownDEPRECATED [@value 2109] *)
    (* | PocketUniversesLocalizationDEPRECATED [@value 2110] *)
    | ClassMethNonFinalSelf [@value 2111]
    | ParentInFunctionPointer [@value 2112]
    | SelfInNonFinalFunctionPointer [@value 2113]
    | ClassMethNonFinalCLASS [@value 2114]
    | WildcardTypeParamDisallowed [@value 2115]
    (* | CallingAssertDEPRECATED [@value 2116] *)
    | InvalidWildcardContext [@value 2117]
    | ExplicitConsistentConstructor [@value 2118]
    | InvalidReqClass [@value 2119]
    | ModuleDeclarationOutsideAllowedFiles [@value 2120]
    | DynamicMethodAccess [@value 2121]
    (* | TypeConstantInEnumClassOutsideAllowedLocationsDEPRECATED [@value 2122] *)
    | InvalidBuiltinType [@value 2123]
    | InvalidMemoizeLabel [@value 2124]
    | DynamicHintDisallowed [@value 2125]
    | IllegalTypedLocal [@value 2126]
    | InternalModuleLevelTrait [@value 2127]
    | ToplevelStatement [@value 2128]
    | InvalidTypeAccessInWhere [@value 2129]
    | AttributeOutsideAllowedFiles [@value 2130]
    | HhiAttributeRestriction [@value 2131]
    | TypedOpenShapeDisallowed [@value 2132]
    | NamedVariadicTypeDisallowed [@value 2133]
    | VariadicNamedParameterDisallowed [@value 2134]
    | ImplicitPackageFileDirectlyUnderPath [@value 2135]
    | ImplicitPackageInvalidMemberDir [@value 2136]
    | TraitSelfShapeKey [@value 2137]
  (* Add new Naming codes here! Comment out when deprecating. *)
  [@@deriving enum, show { with_path = false }]

  let err_code = to_enum
end

module NastCheck = struct
  type t =
    | AbstractBody [@value 3001]
    | AbstractWithBody [@value 3002]
    | AwaitInSyncFunction [@value 3003]
    | CallBeforeInit [@value 3004]
    | CaseFallthrough [@value 3005]
    | ContinueInSwitch [@value 3006]
    (* | DangerousMethodNameDEPRECATED [@value 3007] *)
    | DefaultFallthrough [@value 3008]
    | InterfaceWithMemberVariable [@value 3009]
    | InterfaceWithStaticMemberVariable [@value 3010]
    | Magic [@value 3011]
    | NoConstructParent [@value 3012]
    | NonInterface [@value 3013]
    (* | NotAbstractWithoutBodyDEPRECATED [@value 3014] *)
    | NotInitialized [@value 3015]
    (* | NotPublicInterfaceDEPRECATED [@value 3016] *)
    | RequiresNonClass [@value 3017]
    | ReturnInFinally [@value 3018]
    (* | ReturnInGenDEPRECATED [@value 3019] *)
    | ToStringReturnsString [@value 3020]
    | ToStringVisibility [@value 3021]
    | ToplevelBreak [@value 3022]
    | ToplevelContinue [@value 3023]
    | UsesNonTrait [@value 3024]
    | IllegalFunctionName [@value 3025]
    | NotAbstractWithoutTypeconst [@value 3026]
    | TypeconstDependsOnExternalTparam [@value 3027]
    (* | TypeconstAssignedTparamDEPRECATED [@value 3028] *)
    (* | AbstractWithTypeconstDEPRECATED [@value 3029] *)
    | ConstructorRequired [@value 3030]
    | InterfaceWithPartialTypeconst [@value 3031]
    | MultipleXhpCategory [@value 3032]
    (* | OptionalShapeFieldsNotSupportedDEPRECATED [@value 3033] *)
    (* | AwaitNotAllowedDEPRECATED [@value 3034] *)
    (* | AsyncInInterfaceDEPRECATED [@value 3035] *)
    (* | AwaitInCoroutineDEPRECATED [@value 3036] *)
    (* | YieldInCoroutineDEPRECATED [@value 3037] *)
    (* | SuspendOutsideOfCoroutineDEPRECATED [@value 3038] *)
    (* | SuspendInFinallyDEPRECATED [@value 3039] *)
    (* | BreakContinueNNotSupportedDEPRECATED [@value 3040] *)
    (* | StaticMemoizedFunctionDEPRECATED [@value 3041] *)
    (* | InoutParamsInCoroutineDEPRECATED [@value 3042] *)
    | InoutParamsSpecial [@value 3043]
    (* | InoutParamsMixByrefDEPRECATED [@value 3044] *)
    | InoutParamsMemoize [@value 3045]
    (* | InoutParamsRetByRefDEPRECATED [@value 3046] *)
    | ReadingFromAppend [@value 3047]
    (* | ConstAttributeProhibitedDEPRECATED [@value 3048] *)
    (* | GlobalInReactiveContextDEPRECATED [@value 3049] *)
    (* | InoutArgumentBadExprDEPRECATED [@value 3050] *)
    (* | MutableParamsOutsideOfSyncDEPRECATED [@value 3051] *)
    (* | MutableAsyncMethodDEPRECATED [@value 3052] *)
    (* | MutableMethodsMustBeReactiveDEPRECATED [@value 3053] *)
    (* | MutableAttributeOnFunctionDEPRECATED [@value 3054] *)
    (* | MutableReturnAnnotatedDeclsMustBeReactiveDEPRECATED [@value 3055] *)
    | IllegalDestructor [@value 3056]
    (* | ConditionallyReactiveFunctionDEPRECATED [@value 3057] *)
    (* | MultipleConditionallyReactiveAnnotationsDEPRECATED [@value 3058] *)
    (* | ConditionallyReactiveAnnotationInvalidArgumentsDEPRECATED [@value 3059] *)
    (* | MissingReactivityForConditionDEPRECATED [@value 3060] *)
    (* | MultipleReactivityAnnotationsDEPRECATED [@value 3061] *)
    (* | RxIsEnabledInvalidLocationDEPRECATED [@value 3062] *)
    (* | MaybeRxInvalidLocationDEPRECATED [@value 3063] *)
    (* | NoOnlyrxIfRxfuncForRxIfArgsDEPRECATED [@value 3064] *)
    (* | CoroutineInConstructorDEPRECATED [@value 3065] *)
    (* | IllegalReturnByRefDEPRECATED [@value 3066] *)
    (* | IllegalByRefExprDEPRECATED [@value 3067] *)
    (* | VariadicByRefParamDEPRECATED [@value 3068] *)
    (* | MaybeMutableAttributeOnFunctionDEPRECATED [@value 3069] *)
    (* | ConflictingMutableAndMaybeMutableAttributesDEPRECATED [@value 3070] *)
    (* | MaybeMutableMethodsMustBeReactiveDEPRECATED [@value 3071] *)
    | RequiresFinalClass [@value 3072]
    | InterfaceUsesTrait [@value 3073]
    | NonstaticMethodInAbstractFinalClass [@value 3074]
    (* | MutableOnStaticDEPRECATED [@value 3075] *)
    (* | ClassnameConstInstanceOfDEPRECATED [@value 3076] *)
    (* | ByRefParamOnConstructDEPRECATED [@value 3077] *)
    (* | ByRefDynamicCallDEPRECATED [@value 3078] *)
    (* | ByRefPropertyDEPRECATED [@value 3079] *)
    (* | ByRefCallDEPRECATED [@value 3080] *)
    (* | SwitchNonTerminalDefaultDEPRECATED [@value 3081] *)
    (* | SwitchMultipleDefaultDEPRECATED [@value 3082] *)
    | RepeatedRecordFieldName [@value 3083]
    | PhpLambdaDisallowed [@value 3084]
    | EntryPointArguments [@value 3085]
    | VariadicMemoize [@value 3086]
    | AbstractMethodMemoize [@value 3087]
    | InstancePropertyInAbstractFinalClass [@value 3088]
    | DynamicallyCallableReified [@value 3089]
    | IllegalContext [@value 3090]
    (* | InvalidConstFunAttributeDEPRECATED [@value 3091] *)
    | ListRvalue [@value 3092]
    | PartiallyAbstractTypeconstDefinition [@value 3093]
    | EntryPointGenerics [@value 3094]
    | InternalProtectedOrPrivate [@value 3095]
    | InoutInTransformedPsuedofunction [@value 3096]
    | PrivateAndFinal [@value 3097]
    (* | InternalOutsideModuleDEPRECATED [@value 3098] *)
    | InternalMemberInsidePublicTrait [@value 3099]
    | AttributeConflictingMemoize [@value 3100]
    | RefinementInTypeStruct [@value 3101]
    | Soft_internal_without_internal [@value 3102]
    | CloneReturnType [@value 3104]
    (* | PackageExprInInvariantDEPRECATED [@value 3105] *)
    | NamedInTransformedPseudofunction [@value 3106]
    | RequirePackageStrictInclusion [@value 3107]
    | ClassSealedWithTrait [@value 3108]
    | PackageExpressionStrictInclusion [@value 3109]
    | ObservationNotAllowedForPackage [@value 3110]
    | PackageOverrideTargetNotIncluded [@value 3111]
    | RedundantPackageOverride [@value 3112]
    (* | NamedParamsMemoizeDEPRECATED [@value 3113] *)
    | OverrideNotAllowedForPackage [@value 3114]
    | PlaceholderRead [@value 3115]
    | PlaceholderInoutWrite [@value 3116]
  (* Add new NastCheck codes here! Comment out when deprecating. *)
  [@@deriving enum, show { with_path = false }]

  let err_code = to_enum
end

module Typing = struct
  type t =
    | InternalError [@value 0]
    (* | AbstractClassFinalDEPRECATED [@value 4001] *)
    | UninstantiableClass [@value 4002]
    (* | AnonymousRecursiveDEPRECATED [@value 4003] *)
    (* | AnonymousRecursiveCallDEPRECATED [@value 4004] *)
    | ArrayAccessRead [@value 4005]
    | ArrayAppend [@value 4006]
    | ArrayCast [@value 4007]
    | ArrayGetArity [@value 4008]
    | BadCall [@value 4009]
    (* | ClassArityDEPRECATED [@value 4010] *)
    | ConstMutation [@value 4011]
    | ConstructorNoArgs [@value 4012]
    | CyclicClassDef [@value 4013]
    | CyclicTypedef [@value 4014]
    | DiscardedAwaitable [@value 4015]
    | IssetEmptyInStrict [@value 4016]
    (* | DynamicYieldPrivateDEPRECATED [@value 4017] *)
    | EnumConstantTypeBad [@value 4018]
    | EnumSwitchNonexhaustive [@value 4019]
    | EnumSwitchNotConst [@value 4020]
    | EnumSwitchRedundant [@value 4021]
    | EnumSwitchRedundantDefault [@value 4022]
    | EnumSwitchWrongClass [@value 4023]
    | EnumTypeBad [@value 4024]
    (* | EnumTypeTypedefMixedDEPRECATED [@value 4025] *)
    | ExpectedClass [@value 4026]
    | ExpectedLiteralFormatString [@value 4027]
    (* | ExpectedStaticIntDEPRECATED [@value 4028] *)
    | ExpectedTparam [@value 4029]
    | ExpectingReturnTypeHint [@value 4030]
    (* | ExpectingReturnTypeHintSuggestDEPRECATED [@value 4031] *)
    | ExpectingTypeHint [@value 4032]
    | ExpectingTypeHintVariadic [@value 4033]
    (* | RetiredError4034DEPRECATED [@value 4034] *)
    | ExtendFinal [@value 4035]
    | FieldKinds [@value 4036]
    (* | FieldMissingDEPRECATED [@value 4037] *)
    | FormatString [@value 4038]
    (* | FunArityMismatchDEPRECATED [@value 4039] *)
    | FunTooFewArgs [@value 4040]
    | FunTooManyArgs [@value 4041]
    | FunUnexpectedNonvariadic [@value 4042]
    | FunVariadicityHhVsPhp56 [@value 4043]
    (* | GenaExpectsArrayDEPRECATED [@value 4044] *)
    (* | GenericArrayStrictDEPRECATED [@value 4045] *)
    | GenericStatic [@value 4046]
    | ImplementAbstract [@value 4047]
    (* | InterfaceFinalDEPRECATED [@value 4048] *)
    | InvalidShapeFieldConst [@value 4049]
    | InvalidShapeFieldLiteral [@value 4050]
    | InvalidShapeFieldName [@value 4051]
    | InvalidShapeFieldType [@value 4052]
    | MemberNotFound [@value 4053]
    | MemberNotImplemented [@value 4054]
    | MissingAssign [@value 4055]
    | MissingConstructor [@value 4056]
    | MissingField [@value 4057]
    (* | NegativeTupleIndexDEPRECATED [@value 4058] *)
    | SelfOutsideClass [@value 4059]
    | NewStaticInconsistent [@value 4060]
    | StaticOutsideClass [@value 4061]
    | NonObjectMemberRead [@value 4062]
    | NullContainer [@value 4063]
    | NullMemberRead [@value 4064]
    (* | NullableParameterDEPRECATED [@value 4065] *)
    | OptionReturnOnlyTypehint [@value 4066]
    | ObjectString [@value 4067]
    (* | OptionMixedDEPRECATED [@value 4068] *)
    (* | OverflowDEPRECATED [@value 4069] *)
    | OverrideFinal [@value 4070]
    | OverridePerTrait [@value 4071]
    (* | PairArityDEPRECATED [@value 4072] *)
    | AbstractCall [@value 4073]
    | ParentInTrait [@value 4074]
    | ParentOutsideClass [@value 4075]
    | ParentUndefined [@value 4076]
    | PreviousDefault [@value 4077]
    (* | PrivateClassMethDEPRECATED [@value 4078] *)
    (* | PrivateInstMethDEPRECATED [@value 4079] *)
    (* | PrivateOverrideDEPRECATED [@value 4080] *)
    (* | ProtectedClassMethDEPRECATED [@value 4081] *)
    (* | ProtectedInstMethDEPRECATED [@value 4082] *)
    | ReadBeforeWrite [@value 4083]
    | ReturnInVoid [@value 4084]
    | ShapeFieldClassMismatch [@value 4085]
    | ShapeFieldTypeMismatch [@value 4086]
    | ShouldNotBeOverride [@value 4087]
    (* | SketchyNullCheckDEPRECATED [@value 4088] *)
    (* | SketchyNullCheckPrimitiveDEPRECATED [@value 4089] *)
    | SmemberNotFound [@value 4090]
    | StaticDynamic [@value 4091]
    (* | StaticOverflowDEPRECATED [@value 4092] *)
    (* | RetiredError4093DEPRECATED [@value 4093] *)
    (* | ThisInStaticDEPRECATED [@value 4094] *)
    | ThisVarOutsideClass [@value 4095]
    (* | TraitFinalDEPRECATED [@value 4096] *)
    (* | TupleArityDEPRECATED [@value 4097] *)
    (* | TupleArityMismatchDEPRECATED [@value 4098] *)
    (* | TupleIndexTooLargeDEPRECATED [@value 4099] *)
    | TupleSyntax [@value 4100]
    | TypeArityMismatch [@value 4101]
    (* | TypeParamArityDEPRECATED [@value 4102] *)
    (* | RetiredError4103DEPRECATED [@value 4103] *)
    | TypingTooFewArgs [@value 4104]
    | TypingTooManyArgs [@value 4105]
    | UnboundGlobal [@value 4106]
    | UnboundNameTyping [@value 4107]
    | UndefinedField [@value 4108]
    (* | UndefinedParentDEPRECATED [@value 4109] *)
    | UnifyError [@value 4110]
    | UnsatisfiedReq [@value 4111]
    | Visibility [@value 4112]
    | VisibilityExtends [@value 4113]
    (* | VoidParameterDEPRECATED [@value 4114] *)
    | WrongExtendKind [@value 4115]
    | GenericUnify [@value 4116]
    (* | NullsafeNotNeededDEPRECATED [@value 4117] *)
    | TrivialStrictEq [@value 4118]
    | VoidUsage [@value 4119]
    | DeclaredCovariant [@value 4120]
    | DeclaredContravariant [@value 4121]
    (* | UnsetInStrictDEPRECATED [@value 4122] *)
    (* | StrictMembersNotKnownDEPRECATED [@value 4123] *)
    | ErasedGenericAtRuntime [@value 4124]
    (* | DynamicClassDEPRECATED [@value 4125] *)
    | AttributeTooManyArguments [@value 4126]
    | AttributeParamType [@value 4127]
    | DeprecatedUse [@value 4128]
    | AbstractConstUsage [@value 4129]
    | CannotDeclareConstant [@value 4130]
    | CyclicTypeconst [@value 4131]
    | NullsafePropertyWriteContext [@value 4132]
    | NoreturnUsage [@value 4133]
    (* | ThisLvalueDEPRECATED [@value 4134] *)
    | UnsetNonidxInStrict [@value 4135]
    | InvalidShapeFieldNameEmpty [@value 4136]
    (* | InvalidShapeFieldNameNumberDEPRECATED [@value 4137] *)
    | ShapeFieldsUnknown [@value 4138]
    | InvalidShapeRemoveKey [@value 4139]
    (* | MissingOptionalFieldDEPRECATED [@value 4140] *)
    (* | ShapeFieldUnsetDEPRECATED [@value 4141] *)
    | AbstractConcreteOverride [@value 4142]
    | LocalVariableModifedAndUsed [@value 4143]
    | LocalVariableModifedTwice [@value 4144]
    | AssignDuringCase [@value 4145]
    | CyclicEnumConstraint [@value 4146]
    | UnpackingDisallowed [@value 4147]
    | InvalidClassname [@value 4148]
    | InvalidMemoizedParam [@value 4149]
    | IllegalTypeStructure [@value 4150]
    | NotNullableCompareNullTrivial [@value 4151]
    (* | ClassPropertyOnlyStaticLiteralDEPRECATED [@value 4152] *)
    | AttributeTooFewArguments [@value 4153]
    (* | ReferenceExprDEPRECATED [@value 4154] *)
    | UnificationCycle [@value 4155]
    | KeysetSet [@value 4156]
    | EqIncompatibleTypes [@value 4157]
    | ContravariantThis [@value 4158]
    (* | InstanceofAlwaysFalseDEPRECATED [@value 4159] *)
    (* | InstanceofAlwaysTrueDEPRECATED [@value 4160] *)
    (* | AmbiguousMemberDEPRECATED [@value 4161] *)
    (* | InstanceofGenericClassnameDEPRECATED [@value 4162] *)
    | RequiredFieldIsOptional [@value 4163]
    (* | FinalPropertyDEPRECATED [@value 4164] *)
    | ArrayGetWithOptionalField [@value 4165]
    (* | UnknownFieldDisallowedInShapeDEPRECATED [@value 4166] *)
    | NullableCast [@value 4167]
    (* | PassByRefAnnotationMissingDEPRECATED [@value 4168] *)
    (* | NonCallArgumentInSuspendDEPRECATED [@value 4169] *)
    (* | NonCoroutineCallInSuspendDEPRECATED [@value 4170] *)
    (* | CoroutineCallOutsideOfSuspendDEPRECATED [@value 4171] *)
    (* | FunctionIsNotCoroutineDEPRECATED [@value 4172] *)
    (* | CoroutinnessMismatchDEPRECATED [@value 4173] *)
    (* | ExpectingAwaitableReturnTypeHintDEPRECATED [@value 4174] *)
    (* | ReffinessInvariantDEPRECATED [@value 4175] *)
    (* | DollardollarLvalueDEPRECATED [@value 4176] *)
    (* | StaticMethodOnInterfaceDEPRECATED [@value 4177] *)
    | DuplicateUsingVar [@value 4178]
    | IllegalDisposable [@value 4179]
    | EscapingDisposable [@value 4180]
    (* | PassByRefAnnotationUnexpectedDEPRECATED [@value 4181] *)
    | InoutAnnotationMissing [@value 4182]
    | InoutAnnotationUnexpected [@value 4183]
    | InoutnessMismatch [@value 4184]
    | StaticSyntheticMethod [@value 4185]
    | TraitReuse [@value 4186]
    | InvalidNewDisposable [@value 4187]
    | EscapingDisposableParameter [@value 4188]
    | AcceptDisposableInvariant [@value 4189]
    | InvalidDisposableHint [@value 4190]
    | XhpRequired [@value 4191]
    | EscapingThis [@value 4192]
    | IllegalXhpChild [@value 4193]
    | MustExtendDisposable [@value 4194]
    | InvalidIsAsExpressionHint [@value 4195]
    | AssigningToConst [@value 4196]
    | SelfConstParentNot [@value 4197]
    (* | ParentConstSelfNotDEPRECATED [@value 4198] *)
    (* | PartiallyValidIsAsExpressionHintDEPRECATED [@value 4199] *)
    (* | NonreactiveFunctionCallDEPRECATED [@value 4200] *)
    (* | NonreactiveIndexingDEPRECATED [@value 4201] *)
    (* | ObjSetReactiveDEPRECATED [@value 4202] *)
    (* | FunReactivityMismatchDEPRECATED [@value 4203] *)
    | OverridingPropConstMismatch [@value 4204]
    | InvalidReturnDisposable [@value 4205]
    | InvalidDisposableReturnHint [@value 4206]
    | ReturnDisposableMismatch [@value 4207]
    | InoutArgumentBadType [@value 4208]
    (* | InconsistentUnsetDEPRECATED [@value 4209] *)
    (* | ReassignMutableVarDEPRECATED [@value 4210] *)
    (* | InvalidFreezeTargetDEPRECATED [@value 4211] *)
    (* | InvalidFreezeUseDEPRECATED [@value 4212] *)
    (* | FreezeInNonreactiveContextDEPRECATED [@value 4213] *)
    (* | MutableCallOnImmutableDEPRECATED [@value 4214] *)
    (* | MutableArgumentMismatchDEPRECATED [@value 4215] *)
    (* | InvalidMutableReturnResultDEPRECATED [@value 4216] *)
    (* | MutableReturnResultMismatchDEPRECATED [@value 4217] *)
    (* | NonreactiveCallFromShallowDEPRECATED [@value 4218] *)
    | EnumTypeTypedefNonnull [@value 4219]
    (* | RxEnabledInNonRxContextDEPRECATED [@value 4220] *)
    (* | RxEnabledInLambdasDEPRECATED [@value 4221] *)
    | AmbiguousLambda [@value 4222]
    (* | EllipsisStrictModeDEPRECATED [@value 4223] *)
    (* | UntypedLambdaStrictModeDEPRECATED [@value 4224] *)
    (* | BindingRefInArrayDEPRECATED [@value 4225] *)
    (* | OutputInWrongContextDEPRECATED [@value 4226] *)
    (* | SuperglobalInReactiveContextDEPRECATED [@value 4227] *)
    (* | StaticPropertyInWrongContextDEPRECATED [@value 4228] *)
    (* | StaticInReactiveContextDEPRECATED [@value 4229] *)
    (* | GlobalInReactiveContextDEPRECATED [@value 4230] *)
    | WrongExpressionKindAttribute [@value 4231]
    (* | AttributeClassNoConstructorArgsDEPRECATED [@value 4232] *)
    (* | InvalidTypeForOnlyrxIfRxfuncParameterDEPRECATED [@value 4233] *)
    (* | MissingAnnotationForOnlyrxIfRxfuncParameterDEPRECATED [@value 4234] *)
    (* | CannotReturnBorrowedValueAsImmutableDEPRECATED [@value 4235] *)
    (* | DeclOverrideMissingHintDEPRECATED [@value 4236] *)
    (* | InvalidConditionallyReactiveCallDEPRECATED [@value 4237] *)
    | ExtendSealed [@value 4238]
    (* | SealedFinalDEPRECATED [@value 4239] *)
    | ComparisonInvalidTypes [@value 4240]
    (* | OptionVoidDEPRECATED [@value 4241] *)
    (* | MutableInNonreactiveContextDEPRECATED [@value 4242] *)
    (* | InvalidArgumentOfRxMutableFunctionDEPRECATED [@value 4243] *)
    (* | LetVarImmutabilityViolationDEPRECATED [@value 4244] *)
    (* | UnsealableDEPRECATED [@value 4245] *)
    (* | ReturnVoidToRxMismatchDEPRECATED [@value 4246] *)
    (* | ReturnsVoidToRxAsNonExpressionStatementDEPRECATED [@value 4247] *)
    (* | NonawaitedAwaitableInReactiveContextDEPRECATED [@value 4248] *)
    | ShapesKeyExistsAlwaysTrue [@value 4249]
    | ShapesKeyExistsAlwaysFalse [@value 4250]
    | ShapesMethodAccessWithNonExistentField [@value 4251]
    | NonClassMember [@value 4252]
    (* | PassingArrayCellByRefDEPRECATED [@value 4253] *)
    (* | CallSiteReactivityMismatchDEPRECATED [@value 4254] *)
    (* | RxParameterConditionMismatchDEPRECATED [@value 4255] *)
    | AmbiguousObjectAccess [@value 4256]
    (* | ExtendPPLDEPRECATED [@value 4257] *)
    (* | ReassignMaybeMutableVarDEPRECATED [@value 4258] *)
    (* | MaybeMutableArgumentMismatchDEPRECATED [@value 4259] *)
    (* | ImmutableArgumentMismatchDEPRECATED [@value 4260] *)
    (* | ImmutableCallOnMutableDEPRECATED [@value 4261] *)
    (* | InvalidCallMaybeMutableDEPRECATED [@value 4262] *)
    (* | MutabilityMismatchDEPRECATED [@value 4263] *)
    (* | InvalidPPLCallDEPRECATED [@value 4264] *)
    (* | InvalidPPLStaticCallDEPRECATED [@value 4265] *)
    (* | TypeTestInLambdaDEPRECATED [@value 4266] *)
    (* | InvalidTraversableInRxDEPRECATED [@value 4267] *)
    (* | ReassignMutableThisDEPRECATED [@value 4268] *)
    (* | MutableExpressionAsMultipleMutableArgumentsDEPRECATED [@value 4269] *)
    (* | InvalidUnsetTargetInRxDEPRECATED [@value 4270] *)
    (* | CoroutineOutsideExperimentalDEPRECATED [@value 4271] *)
    (* | PPLMethPointerDEPRECATED [@value 4272] *)
    (* | InvalidTruthinessTestDEPRECATED [@value 4273] *)
    | RePrefixedNonString [@value 4274]
    | BadRegexPattern [@value 4275]
    (* | SketchyTruthinessTestDEPRECATED [@value 4276] *)
    | LateInitWithDefault [@value 4277]
    (* | OverrideMemoizeLSBDEPRECATED [@value 4278] *)
    | ClassVarTypeGenericParam [@value 4279]
    (* | InvalidSwitchCaseValueTypeDEPRECATED [@value 4280] *)
    | StringCast [@value 4281]
    | BadLateInitOverride [@value 4282]
    (* | EscapingMutableObjectDEPRECATED [@value 4283] *)
    | OverrideLSB [@value 4284]
    | MultipleConcreteDefs [@value 4285]
    (* | MoveInNonreactiveContextDEPRECATED [@value 4286] *)
    (* | InvalidMoveUseDEPRECATED [@value 4287] *)
    (* | InvalidMoveTargetDEPRECATED [@value 4288] *)
    (* | IgnoredResultOfFreezeDEPRECATED [@value 4289] *)
    (* | IgnoredResultOfMoveDEPRECATED [@value 4290] *)
    | UnexpectedTy [@value 4291]
    (* | UnserializableType [@value 4292] *)
    (* | InconsistentMutabilityDEPRECATED [@value 4293] *)
    (* | InvalidMutabilityFlavorInAssignmentDEPRECATED [@value 4294] *)
    (* | OptionNullDEPRECATED [@value 4295] *)
    | UnknownObjectMember [@value 4296]
    | UnknownType [@value 4297]
    (* | InvalidArrayKeyReadDEPRECATED [@value 4298] *)
    (* | ReferenceExprNotFunctionArgDEPRECATED [@value 4299] *)
    (* | RedundantRxConditionDEPRECATED [@value 4300] *)
    | RedeclaringMissingMethod [@value 4301]
    | InvalidEnforceableTypeArgument [@value 4302]
    | RequireArgsReify [@value 4303]
    | TypecheckerTimeout [@value 4304]
    | InvalidReifiedArgument [@value 4305]
    | GenericsNotAllowed [@value 4306]
    | InvalidNewableTypeArgument [@value 4307]
    | InvalidNewableTypeParamConstraints [@value 4308]
    | NewWithoutNewable [@value 4309]
    | NewClassReified [@value 4310]
    | MemoizeReified [@value 4311]
    | ConsistentConstructReified [@value 4312]
    | MethodVariance [@value 4313]
    | MissingXhpRequiredAttr [@value 4314]
    | BadXhpAttrRequiredOverride [@value 4315]
    (* | ReifiedTparamVariadicDEPRECATED [@value 4316] *)
    | UnresolvedTypeVariable [@value 4317]
    | InvalidSubString [@value 4318]
    | InvalidArrayKeyConstraint [@value 4319]
    | OverrideNoDefaultTypeconst [@value 4320]
    | ShapeAccessWithNonExistentField [@value 4321]
    | DisallowPHPArraysAttr [@value 4322]
    | TypeConstraintViolation [@value 4323]
    | IndexTypeMismatch [@value 4324]
    | ExpectedStringlike [@value 4325]
    | TypeConstantMismatch [@value 4326]
    (* | TypeConstantRedeclarationDEPRECATED [@value 4327] *)
    | ConstantDoesNotMatchEnumType [@value 4328]
    | EnumConstraintMustBeArraykey [@value 4329]
    | EnumSubtypeMustHaveCompatibleConstraint [@value 4330]
    | ParameterDefaultValueWrongType [@value 4331]
    | NewtypeAliasMustSatisfyConstraint [@value 4332]
    (* | BadFunctionTypevarDEPRECATED [@value 4333] *)
    (* | BadClassTypevarDEPRECATED [@value 4334] *)
    (* | BadMethodTypevarDEPRECATED [@value 4335] *)
    | MissingReturnInNonVoidFunction [@value 4336]
    | InoutReturnTypeMismatch [@value 4337]
    | ClassConstantValueDoesNotMatchHint [@value 4338]
    | ClassPropertyInitializerTypeDoesNotMatchHint [@value 4339]
    | BadDeclOverride [@value 4340]
    | BadMethodOverride [@value 4341]
    | BadEnumExtends [@value 4342]
    | XhpAttributeValueDoesNotMatchHint [@value 4343]
    | TraitPropConstClass [@value 4344]
    | EnumUnderlyingTypeMustBeArraykey [@value 4345]
    | ClassGetReified [@value 4346]
    | RequireGenericExplicit [@value 4347]
    | ClassConstantTypeMismatch [@value 4348]
    (* | PocketUniversesExpansionDEPRECATED [@value 4349] *)
    (* | PocketUniversesTypingDEPRECATED [@value 4350] *)
    (* | RecordInitValueDoesNotMatchHintDEPRECATED [@value 4351] *)
    | AbstractTconstNotAllowed [@value 4352]
    (* | NewAbstractRecordDEPRECATED [@value 4353] *)
    (* | RecordMissingRequiredFieldDEPRECATED [@value 4354] *)
    (* | RecordUnknownFieldDEPRECATED [@value 4355] *)
    (* | CyclicRecordDefDEPRECATED [@value 4356] *)
    | InvalidDestructure [@value 4357]
    | StaticMethWithClassReifiedGeneric [@value 4358]
    | SplatArrayRequired [@value 4359]
    (* | SplatArrayVariadicDEPRECATED [@value 4360] *)
    | ExceptionOccurred [@value 4361]
    | InvalidReifiedFunctionPointer [@value 4362]
    | BadFunctionPointerConstruction [@value 4363]
    (* | NotARecordDEPRECATED [@value 4364] *)
    | TraitReuseInsideClass [@value 4365]
    | RedundantGeneric [@value 4366]
    (* | PocketUniversesInvalidUpperBoundsDEPRECATED [@value 4367] *)
    (* | PocketUniversesRefinementDEPRECATED [@value 4368] *)
    (* | PocketUniversesReservedSyntaxDEPRECATED [@value 4369] *)
    | ArrayAccessWrite [@value 4370]
    | InvalidArrayKeyWrite [@value 4371]
    | NullMemberWrite [@value 4372]
    | NonObjectMemberWrite [@value 4373]
    | ConcreteConstInterfaceOverride [@value 4374]
    | MethCallerTrait [@value 4375]
    (* | PocketUniversesAttributesDEPRECATED [@value 4376] *)
    | DuplicateInterface [@value 4377]
    (* | TypeParameterNameAlreadyUsedNonShadowDEPRECATED [@value 4378] *)
    (* | IllegalInformationFlowDEPRECATED [@value 4379] *)
    (* | ContextImplicitPolicyLeakageDEPRECATED [@value 4380] *)
    | ReifiedFunctionReference [@value 4381]
    (* | ClassMethAbstractCall [@value 4382] *)
    | KindMismatch [@value 4383]
    (* | UnboundNameTypeConstantAccessDEPRECATED [@value 4384] *)
    (* | UnknownInformationFlowDEPRECATED [@value 4385] *)
    (* | CallsiteCIPPMismatchDEPRECATED [@value 4386] *)
    (* | NonpureFunctionCallDEPRECATED [@value 4387] *)
    | IncompatibleEnumInclusion [@value 4388]
    | RedeclaringClassishConstant [@value 4389]
    | CallCoeffects [@value 4390]
    | AbstractFunctionPointer [@value 4391]
    | UnnecessaryAttribute [@value 4392]
    | InheritedMethodCaseDiffers [@value 4393]
    | EnumClassLabelUnknown [@value 4394]
    (* | ViaLabelInvalidParameterDEPRECATED [@value 4395] *)
    | EnumClassLabelAsExpression [@value 4396]
    (* | EnumClassLabelInvalidArgumentDEPRECATED [@value 4397] *)
    (* | IFCInternalErrorDEPRECATED [@value 4398] *)
    (* | IFCExternalContravariantDEPRECATED [@value 4399] *)
    (* | IFCPolicyMismatchDEPRECATED [@value 4400] *)
    | OpCoeffects [@value 4401]
    | ImplementsDynamic [@value 4402]
    | SubtypeCoeffects [@value 4403]
    | ImmutableLocal [@value 4404]
    (* | EnumClassesReservedSyntaxDEPRECATED [@value 4405] *)
    | NonsenseMemberSelection [@value 4406]
    | ConsiderMethCaller [@value 4407]
    (* | EnumSupertypingReservedSyntaxDEPRECATED [@value 4408] *)
    | ReadonlyValueModified [@value 4409]
    (* | ReadonlyVarMismatchDEPRECATED [@value 4410] *)
    | ReadonlyMismatch [@value 4411]
    | ExplicitReadonlyCast [@value 4412]
    | ReadonlyMethodCall [@value 4413]
    | StrictStrConcatTypeMismatch [@value 4414]
    | StrictStrInterpTypeMismatch [@value 4415]
    | InvalidMethCallerCallingConvention [@value 4416]
    (* | UnsafeCastDEPRECATED [@value 4417] *)
    | ReadonlyException [@value 4418]
    | InvalidTypeHint [@value 4419]
    | ExperimentalExpressionTrees [@value 4420]
    | ReturnsWithAndWithoutValue [@value 4421]
    | NonVoidAnnotationOnReturnVoidFun [@value 4422]
    | BitwiseMathInvalidArgument [@value 4423]
    | CyclicClassConstant [@value 4424]
    | PrivateDynamicRead [@value 4425]
    | PrivateDynamicWrite [@value 4426]
    | IncDecInvalidArgument [@value 4427]
    | ReadonlyClosureCall [@value 4428]
    | MathInvalidArgument [@value 4429]
    | TypeconstConcreteConcreteOverride [@value 4430]
    | PrivateMethCaller [@value 4431]
    | ProtectedMethCaller [@value 4432]
    | BadConditionalSupportDynamic [@value 4433]
    | ReadonlyInvalidAsMut [@value 4434]
    | InvalidKeysetValue [@value 4435]
    | UnresolvedTypeVariableProjection [@value 4436]
    (* | FunctionPointerWithViaLabelDEPRECATED [@value 4437] *)
    | InvalidEchoArgument [@value 4438]
    | DiamondTraitMethod [@value 4439]
    | ReifiedStaticMethodInExprTree [@value 4440]
    | InvariantViolated [@value 4441]
    | RigidTVarEscape [@value 4442]
    | StrictEqValueIncompatibleTypes [@value 4443]
    | ModuleError [@value 4444]
    (* | SealedNotSubtypeDEPRECATED [@value 4445] *)
    | ModuleHintError [@value 4446]
    | MemoizeObjectWithoutGlobals [@value 4447]
    | ExpressionTreeNonPublicProperty [@value 4448]
    | CovariantIndexTypeMismatch [@value 4449]
    | InoutInPseudofunction [@value 4450]
    | TraitParentConstructInconsistent [@value 4451]
    | HHExpectEquivalentFailure [@value 4452]
    | HHExpectFailure [@value 4453]
    | CallLvalue [@value 4454]
    | UnsafeCastAwait [@value 4455]
    | ThisFinal [@value 4457]
    (* | ExactClassFinalDEPRECATED [@value 4458] *)
    (* | GlobalVariableWriteDEPRECATED [@value 4459] *)
    (* | GlobalVariableInFunctionCallDEPRECATED [@value 4460] *)
    (* | MemoizedFunctionCallDEPRECATED [@value 4461] *)
    | DiamondTraitProperty [@value 4462]
    | ConstructNotInstanceMethod [@value 4463]
    | InvalidMethCallerReadonlyReturn [@value 4464]
    | AbstractMemberInConcreteClass [@value 4465]
    | TraitNotUsed [@value 4466]
    | OverrideAsync [@value 4467]
    | InexactTConstAccess [@value 4468]
    | UnsupportedRefinement [@value 4469]
    | InvalidClassRefinement [@value 4470]
    | InvalidRefinedConstKind [@value 4471]
    | InvalidCrossPackage [@value 4472]
    | InvalidCrossPackageSoft [@value 4473]
    | AttributeNoAutoDynamic [@value 4474]
    | IllegalCaseTypeVariants [@value 4475]
    | StaticCallOnTraitRequireClass [@value 4476]
    | WrongUseKind [@value 4477]
    | MatchNotExhaustive [@value 4478]
    | MatchOnUnsupportedType [@value 4479]
    | MultipleInstantiationInheritence [@value 4480]
    | InternalMethCaller [@value 4481]
    | ClassPointerToString [@value 4482]
    | InvalidXhpAttributeValue [@value 4483]
    | OptionalParameterNotSupported [@value 4484]
    | InvalidRecursiveType [@value 4485]
    | StaticCallOnTraitRequireThisAs [@value 4486]
    | StringToClassPointer [@value 4488]
    | SwitchNeedsDefault [@value 4489]
    | SimpliHackRunPrompt [@value 4493]
    (* | InvalidReifiedArgumentFIXMEableDEPRECATED [@value 4494] *)
    | SimpliHackEvalError [@value 4495]
    | ProtectedInternalMethCaller [@value 4496]
    | StaticPropOnTrait [@value 4497]
    | RecursiveCaseType [@value 4498]
    | MissingNamedArgs [@value 4499]
    | UnexpectedNamedArgs [@value 4500]
    | DuplicateNamedArgs [@value 4501]
    (* | LambdaNamedParamMismatchDEPRECATED [@value 4502] *)
    | OverrideSealed [@value 4503]
    | ExpectBoolForCondition [@value 4504]
    | NeedsConcreteInFinalClass [@value 4505]
    | NeedsConcreteOnInstanceMethod [@value 4506]
    | NeedsConcreteOnConstructor [@value 4507]
    | ConsistentConstructAbstractExtendsNonAbstract [@value 4508]
    | GatedByFeatureFlag [@value 4509]
    | DestructureMissingEllipsis [@value 4510]
    | DestructureMissingRequiredField [@value 4511]
    | DestructureUnknownField [@value 4512]
    | DestructureTupleArityMismatch [@value 4513]
    | TestsBypassVisibilityOverride [@value 4514]
    | TestsBypassVisibilityOnMemberWithoutClass [@value 4515]
    | TestsBypassVisibilityAbstractMissingAttr [@value 4516]
    | ThisAsFunctionPointerParam [@value 4517]
    | StrictIsolationExcludedPathAccess [@value 4518]
    | EnumDuplicateValue [@value 4519]
    | EnumUncheckableValue [@value 4520]
    (* | InvalidUnsetTargetDEPRECATED [@value 4521] *)
    | SplatMayRequireFields [@value 4522]
    | SplatFieldNotKnown [@value 4523]
    | RedeclaringPromotedProperty [@value 4524]
    | RequireDynamicObjGet [@value 4525]
    | NeedsConcreteOverride [@value 4526]
    | InvalidMethCallerNamedParameter [@value 4527]
    | CallNeedsConcrete [@value 4528]
    | AbstractAccessViaStatic [@value 4529]
    | UninstantiableClassViaStatic [@value 4530]
    | RedundantShapeSplat [@value 4531]
    | UnsupportedCyclicShapeSplatBounds [@value 4532]
    | ShapeKeyClassConstMissingAttribute [@value 4533]
    | ShapeKeyClassConstValueUnresolvable [@value 4534]
    | ShapeKeyClassConstIsAbstract [@value 4535]
    | ThisRefinementInGenericClass [@value 4536]
    | PrivateMemberVariance [@value 4537]
    | MemoizeErasedGenericReturn [@value 4538]
  (* Add new Typing codes here! Comment out when deprecating. *)
  [@@deriving enum, show { with_path = false }]

  let err_code = to_enum
end

module Warning = struct
  type t =
    | SketchyEquality [@value 12001]
    (* | SketchyNullCheckDEPRECATED [@value 12003] *)
    | NonDisjointCheck [@value 12004]
    | CastNonPrimitive [@value 12005]
    | TruthinessTest [@value 12006]
    | EqualityCheck [@value 12007]
    | DuplicateProperties [@value 12008]
    | IsIsAlwaysTrue [@value 12009]
    | IsIsAlwaysFalse [@value 12010]
    | AsAlwaysSucceeds [@value 12011]
    | AsAlwaysFails [@value 12012]
    | ClassPointerToString [@value 12013]
    | NoDisjointUnion [@value 12017]
    | SafeAbstractConstAccess [@value 12018]
    | SwitchRedundancy [@value 12019]
    | StaticCallOnTrait [@value 12020]
    | StaticPropertyOverride [@value 12021]
    | StringToClassPointer [@value 12022]
    | NullCoalesceAlways [@value 12023]
    | CallNeedsConcrete [@value 12024]
    (* | AbstractAccessViaStaticDEPRECATED [@value 12025] *)
    (* | UninstantiableClassViaStaticDEPRECATED [@value 12026] *)
    (* | NeedsConcreteOverrideDEPRECATED [@value 12027] *)
    | ExpectBoolForCondition [@value 12028]
    | RedundantNullsafeMemberSelect [@value 12029]
    | NullsafeMemberSelectOnNull [@value 12030]
    | RedundantNullsafePipe [@value 12031]
    | NullsafePipeOnNull [@value 12032]
    | UnboundNameWarning [@value 12033]
    | SetOrKeysetArrayGet [@value 12034]
    | SealedNotSubtype [@value 12035]
    | TanyFound [@value 12036]
    | ConsistentConstructAbstractFinal [@value 12037]
    | DynamicMethodCall [@value 12038]
    | DynamicFunctionCall [@value 12039]
    | DynamicArrayAccess [@value 12040]
    | SealedNotOverride [@value 12041]
    | DynamicPropertyAccess [@value 12042]
    | RedundantRequireThisAs [@value 12043]
    | DynamicReturn [@value 12044]
    | ReadonlyPropertyOverride [@value 12045]
    (* | ShapeKeyClassConstValueUnresolvableDEPRECATED [@value 12046] *)
    (* | ShapeKeyEnumConstValueUnresolvableDEPRECATED [@value 12047] *)
    | RedundantConcurrent [@value 12048]
  (* Add new Warning codes here! Comment out when deprecating. *)
  [@@deriving enum, ord, show { with_path = false }]
end

module Warning_set = Stdlib.Set.Make (Warning)

(* 5xxx: reserved for FB lint *)
(* 6xxx: reserved for FB ai *)
(* 7xxx: reserved for FB ai *)
(* 8xxx: had been used for forward/back-compat; no longer *)
(* 9xxx: reserved for FB ai *)
(* 10xxx: reserved for FB ai *)
(* 11xxx: reserved for global access check (fbcode/hphp/hack/src/typing/tast_check/global_access_check.ml),
 * which is used to detect potential data leaks caused by global variable access.
 * 11001 represents the error when a global variable is definitely written.
 * 11002 represents the warning when a global variable is possibly written via reference.
 * 11003 represents the warning when a global variable is possibly written via function calls.
 * 11004 represents the error when a global variable is definitely read.
 *)
(* 12xxx: reserved for warnings, see module Warning above *)

module GlobalAccessCheck = struct
  type t =
    | DefiniteGlobalWrite [@value 11001]
    | PossibleGlobalWriteViaReference [@value 11002]
    | PossibleGlobalWriteViaFunctionCall [@value 11003]
    | DefiniteGlobalRead [@value 11004]
  (* Add new GlobalAccessCheck codes here! Comment out when deprecating. *)
  [@@deriving enum, show { with_path = false }]

  let err_code : t -> int = to_enum
end
