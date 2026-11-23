(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
module Error_code = Error_codes.Naming
open Naming_error

let visibility_to_string = function
  | Vpublic -> "public"
  | Vprivate -> "private"
  | Vinternal -> "internal"
  | Vprotected -> "protected"
  | Vprotected_internal -> "protected internal"

let const_without_typehint pos name type_ =
  let name = Utils.strip_all_ns name in
  let msg = Printf.sprintf "Please add a type hint `const SomeType %s`" name in
  let (title, new_text) =
    match type_ with
    | "string" -> ("Add string type annotation", "string " ^ name)
    | "int" -> ("Add integer type annotation", "int " ^ name)
    | "float" -> ("Add float type annotation", "float " ^ name)
    | _ -> ("Add mixed type annotation", "mixed " ^ name)
  in
  User_diagnostic.make_err
    Error_code.(to_enum AddATypehint)
    ~quickfixes:[Quickfix.make_eager_default_hint_style ~title ~new_text pos]
    (pos, msg)
    []

let prop_without_typehint pos name vis =
  let visibility = visibility_to_string vis in
  let msg =
    Printf.sprintf "Please add a type hint `%s SomeType $%s`" visibility name
  in
  User_diagnostic.make_err Error_code.(to_enum AddATypehint) (pos, msg) []

let illegal_constant pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalConstant)
    (pos, "Illegal constant value")
    []

let invalid_req_implements pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidReqImplements)
    (pos, "Only traits may use `require implements`")
    []

let invalid_req_extends pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidReqExtends)
    (pos, "Only traits and interfaces may use `require extends`")
    []

let invalid_req_class pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidReqClass)
    (pos, "Only traits may use `require class`")
    []

let invalid_req_constraint pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidReqClass)
    (pos, "Only traits may use `require this <:`")
    []

let did_you_mean_naming pos name suggest_pos suggest_name =
  let name = Render.strip_ns name in
  let suggest_name = Render.strip_ns suggest_name in
  let quickfixes =
    [
      Quickfix.make_eager_default_hint_style
        ~title:("Change to " ^ suggest_name)
        ~new_text:suggest_name
        pos;
    ]
  in
  User_diagnostic.make_err
    Error_code.(to_enum DidYouMeanNaming)
    ~quickfixes
    (pos, "Could not find " ^ Markdown_lite.md_codify name ^ ".")
    [
      Render.suggestion_message
        name
        suggest_name
        (Pos_or_decl.of_raw_pos suggest_pos);
    ]

let using_internal_class pos name =
  User_diagnostic.make_err
    Error_code.(to_enum UsingInternalClass)
    ( pos,
      Markdown_lite.md_codify name
      ^ " is an implementation internal class that cannot be used directly" )
    []

let too_few_type_arguments p =
  User_diagnostic.make_err
    Error_code.(to_enum TooFewTypeArguments)
    (p, "Too few type arguments for this type")
    []

let dynamic_class_name_in_strict_mode pos =
  User_diagnostic.make_err
    Error_code.(to_enum DynamicClassNameInStrictMode)
    (pos, "Cannot use dynamic class or method name in strict mode")
    []

let xhp_optional_required_attr pos id =
  User_diagnostic.make_err
    Error_code.(to_enum XhpOptionalRequiredAttr)
    ( pos,
      "XHP attribute "
      ^ Markdown_lite.md_codify id
      ^ " cannot be marked as nullable and `@required`" )
    []

let wildcard_hint_disallowed pos =
  User_diagnostic.make_err
    Error_code.(to_enum WildcardHintDisallowed)
    (pos, "Wildcard typehints are not allowed in this position")
    []

let dynamic_hint_disallowed pos =
  User_diagnostic.make_err
    Error_code.(to_enum DynamicHintDisallowed)
    (pos, "dynamic typehints are not allowed in this position")
    []

let wildcard_param_disallowed pos =
  User_diagnostic.make_err
    Error_code.(to_enum WildcardTypeParamDisallowed)
    (pos, "Cannot use anonymous type parameter in this position.")
    []

let illegal_use_of_dynamically_callable attr_pos meth_pos vis =
  let visibility = visibility_to_string vis in
  User_diagnostic.make_err
    Error_code.(to_enum IllegalUseOfDynamicallyCallable)
    (attr_pos, "`__DynamicallyCallable` can only be used on public methods")
    [
      ( Pos_or_decl.of_raw_pos meth_pos,
        sprintf "But this method is %s" (Markdown_lite.md_codify visibility) );
    ]

let unexpected_arrow pos cname =
  User_diagnostic.make_err
    Error_code.(to_enum UnexpectedArrow)
    ( pos,
      Format.sprintf {|Keys may not be specified for %s initialization|} cname
    )
    []

let parent_in_function_pointer pos meth_name parent_name =
  let msg =
    match parent_name with
    | None ->
      "Cannot use `parent::` in a function pointer due to class context ambiguity. Consider using the name of the parent class explicitly."
    | Some id ->
      let name =
        Markdown_lite.md_codify (Render.strip_ns id ^ "::" ^ meth_name)
      in
      Format.sprintf
        "Cannot use `parent::` in a function pointer due to class context ambiguity. Consider using %s instead"
        name
  in
  User_diagnostic.make_err
    Error_code.(to_enum ParentInFunctionPointer)
    (pos, msg)
    []

let missing_arrow pos cname =
  User_diagnostic.make_err
    Error_code.(to_enum MissingArrow)
    ( pos,
      Format.sprintf {|Keys must be specified for %s initialization|}
      @@ Markdown_lite.md_codify cname )
    []

let disallowed_xhp_type pos ty_name =
  User_diagnostic.make_err
    Error_code.(to_enum DisallowedXhpType)
    ( pos,
      Format.sprintf {|%s is not a valid type. Use `:xhp` or `XHPChild`.|}
      @@ Markdown_lite.md_codify ty_name )
    []

let name_is_reserved pos name =
  User_diagnostic.make_err
    Error_code.(to_enum NameIsReserved)
    ( pos,
      Format.sprintf {|%s cannot be used as it is reserved.|}
      @@ Markdown_lite.md_codify
      @@ Utils.strip_all_ns name )
    []

let dollardollar_unused pos =
  User_diagnostic.make_err
    Error_code.(to_enum DollardollarUnused)
    ( pos,
      "This expression does not contain a "
      ^ "usage of the special pipe variable. Did you forget to use the `$$` "
      ^ "variable?" )
    []

let already_bound pos name =
  User_diagnostic.make_err
    Error_code.(to_enum NameAlreadyBound)
    ( pos,
      Format.sprintf "Argument already bound: %s"
      @@ Markdown_lite.md_codify name )
    []

let method_name_already_bound pos meth_name =
  User_diagnostic.make_err
    Error_code.(to_enum MethodNameAlreadyBound)
    ( pos,
      Format.sprintf "Method name already bound: %s"
      @@ Markdown_lite.md_codify meth_name )
    []

let error_name_already_bound pos name prev_pos =
  let name = Render.strip_ns name in

  let hhi_msg =
    "This appears to be defined in an hhi file included in your project "
    ^ "root. The hhi files for the standard library are now a part of the "
    ^ "typechecker and must be removed from your project. Typically, you can "
    ^ "do this by deleting the \"hhi\" directory you copied into your "
    ^ "project when first starting with Hack."
  in
  let suffix =
    if Relative_path.(is_hhi (prefix (Pos.filename pos))) then
      [(Pos_or_decl.of_raw_pos prev_pos, hhi_msg)]
    else if Pos.is_hhi prev_pos then
      [(Pos_or_decl.of_raw_pos pos, hhi_msg)]
    else
      []
  in
  let reasons =
    [(Pos_or_decl.of_raw_pos prev_pos, "Previous definition is here")] @ suffix
  in

  User_diagnostic.make_err
    Error_code.(to_enum ErrorNameAlreadyBound)
    ( pos,
      Format.sprintf "Name already bound: %s"
      @@ Markdown_lite.md_codify
      @@ Render.strip_ns name )
    reasons

let invalid_fun_pointer pos name =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidFunPointer)
    ( pos,
      Format.sprintf "%s cannot be used as a function pointer"
      @@ Markdown_lite.md_codify
      @@ Render.strip_ns name )
    []

let undefined pos var_name did_you_mean =
  let (reasons, quickfixes) =
    Option.value_map
      ~default:([], [])
      did_you_mean
      ~f:(fun (did_you_mean, did_you_mean_pos) ->
        ( [
            Render.(
              suggestion_message var_name did_you_mean did_you_mean_pos
              |> Tuple2.map_fst ~f:Pos_or_decl.of_raw_pos);
          ],
          [
            Quickfix.make_eager_default_hint_style
              ~title:("Change to " ^ did_you_mean)
              ~new_text:did_you_mean
              pos;
          ] ))
  in
  User_diagnostic.make_err
    Error_code.(to_enum Undefined)
    ( pos,
      Format.sprintf "Variable %s is undefined, or not always defined."
      @@ Markdown_lite.md_codify var_name )
    reasons
    ~quickfixes

let undefined_in_expr_tree pos var_name dsl did_you_mean =
  let (reasons, quickfixes) =
    Option.value_map
      ~default:([], [])
      did_you_mean
      ~f:(fun (did_you_mean, did_you_mean_pos) ->
        ( [
            ( Pos_or_decl.of_raw_pos did_you_mean_pos,
              Printf.sprintf "Did you forget to splice in `%s`?" did_you_mean );
          ],
          [
            Quickfix.make_eager_default_hint_style
              ~title:("Splice in " ^ did_you_mean)
              ~new_text:(Printf.sprintf "${%s}" did_you_mean)
              pos;
          ] ))
  in
  let dsl =
    match dsl with
    | Some dsl -> Printf.sprintf "`%s` expression tree" @@ Utils.strip_ns dsl
    | None -> "expression tree"
  in
  User_diagnostic.make_err
    Error_code.(to_enum Undefined)
    ( pos,
      Format.sprintf
        "Variable %s is undefined in this %s."
        (Markdown_lite.md_codify var_name)
        dsl )
    reasons
    ~quickfixes

let this_reserved pos =
  User_diagnostic.make_err
    Error_code.(to_enum ThisReserved)
    (pos, "The type parameter `this` is reserved")
    []

let start_with_T pos =
  User_diagnostic.make_err
    Error_code.(to_enum StartWith_T)
    (pos, "Please make your type parameter start with the letter `T` (capital)")
    []

let unexpected_typedef pos expected_kind decl_pos =
  User_diagnostic.make_err
    Error_code.(to_enum UnexpectedTypedef)
    ( pos,
      Printf.sprintf "Expected a %s but got a type alias."
      @@ Markdown_lite.md_codify
      @@ Name_context.to_string expected_kind )
    [(Pos_or_decl.of_raw_pos decl_pos, "Alias definition is here.")]

let field_name_already_bound pos =
  User_diagnostic.make_err
    Error_code.(to_enum FdNameAlreadyBound)
    (pos, "This shape field has the same key as another field")
    []

let named_variadic_type_disallowed pos =
  User_diagnostic.make_err
    Error_code.(to_enum NamedVariadicTypeDisallowed)
    ( pos,
      "Named variadic parameters in function types (e.g. `(function(named int...): void)`) are not enabled."
    )
    []

let variadic_named_parameter_disallowed pos =
  User_diagnostic.make_err
    Error_code.(to_enum VariadicNamedParameterDisallowed)
    (pos, "Variadic named parameters (e.g. `named int...`) are not enabled.")
    []

let trait_self_shape_key pos key_name =
  User_diagnostic.make_err
    Error_code.(to_enum TraitSelfShapeKey)
    ( pos,
      Printf.sprintf
        "`%s` cannot be used as a shape key inside a trait; qualify the declaring class, trait, or interface explicitly"
        key_name )
    []

let primitive_top_level pos =
  User_diagnostic.make_err
    Error_code.(to_enum PrimitiveToplevel)
    ( pos,
      "Primitive type annotations are always available and may no longer be referred to in the toplevel namespace."
    )
    []

let primitive_invalid_alias pos ty_name_used ty_name_canon =
  User_diagnostic.make_err
    Error_code.(to_enum PrimitiveInvalidAlias)
    ( pos,
      Format.sprintf
        "Invalid Hack type. Using %s in Hack is considered an error. Use %s instead, to keep the codebase consistent."
        (Markdown_lite.md_codify ty_name_used)
        (Markdown_lite.md_codify ty_name_canon) )
    []

let dynamic_new_in_strict_mode pos =
  User_diagnostic.make_err
    Error_code.(to_enum DynamicNewInStrictMode)
    (pos, "Cannot use dynamic `new`.")
    []

let invalid_type_access_root pos id_opt =
  let msg =
    match id_opt with
    | Some id ->
      Format.sprintf "%s must be an identifier for a class, `self`, or `this`"
      @@ Markdown_lite.md_codify id
    | _ ->
      Format.sprintf "Type access is only valid for a class, `self`, or `this`"
  in
  User_diagnostic.make_err
    Error_code.(to_enum InvalidTypeAccessRoot)
    (pos, msg)
    []

let invalid_type_access_in_where pos =
  let msg =
    "Type access is only valid for a class, `self`, or `this`."
    ^ " To relate type parameters and type constants,"
    ^ " you likely want to use the 'with refinement' feature instead."
  in
  User_diagnostic.make_err
    Error_code.(to_enum InvalidTypeAccessInWhere)
    (pos, msg)
    []

let duplicate_user_attribute attr_name prev_pos pos =
  User_diagnostic.make_err
    Error_code.(to_enum DuplicateUserAttribute)
    ( pos,
      Format.sprintf "You cannot reuse the attribute %s"
      @@ Markdown_lite.md_codify attr_name )
    [
      ( Pos_or_decl.of_raw_pos prev_pos,
        Markdown_lite.md_codify attr_name ^ " was already used here" );
    ]

let invalid_memoize_label attr_name pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidMemoizeLabel)
    ( pos,
      Format.sprintf
        "%s can only be used with items from MemoizeOption."
        (Markdown_lite.md_codify attr_name) )
    []

let unbound_name_kind = function
  | Name_context.ConstantNamespace -> " (a global constant)"
  | Name_context.FunctionNamespace -> " (a global function)"
  | Name_context.TypeNamespace -> ""
  | Name_context.ClassContext -> " (an object type)"
  | Name_context.TraitContext -> " (a trait)"
  | Name_context.ModuleNamespace -> " (a module)"
  | Name_context.PackageNamespace -> " (a package)"

let unbound_name pos name kind =
  User_diagnostic.make_err
    Error_code.(to_enum UnboundName)
    ( pos,
      "Unbound name: "
      ^ Markdown_lite.md_codify (Render.strip_ns name)
      ^ unbound_name_kind kind )
    []

let unbound_attribute_name pos attr_name closest_attr_name =
  let reason =
    if String.is_prefix attr_name ~prefix:"__" then
      "starts with __ but is not a standard attribute"
    else
      "does not have a class. Please declare a class for the attribute."
  in

  let quickfixes =
    if String.is_prefix attr_name ~prefix:"__" then
      match closest_attr_name with
      | None -> []
      | Some close_name ->
        [
          Quickfix.make_eager_default_hint_style
            ~title:("Change to " ^ Markdown_lite.md_codify close_name)
            ~new_text:close_name
            pos;
        ]
    else
      []
  in

  User_diagnostic.make_err
    Error_code.(to_enum UnboundName)
    ~quickfixes
    ( pos,
      Format.sprintf
        "Unrecognized user attribute: %s %s"
        (Render.strip_ns attr_name |> Markdown_lite.md_codify)
        reason )
    []

let this_no_argument pos =
  User_diagnostic.make_err
    Error_code.(to_enum ThisNoArgument)
    (pos, "`this` type takes no arguments")
    []

let object_cast pos =
  User_diagnostic.make_err
    Error_code.(to_enum ObjectCast)
    (pos, "Casts are only supported for `bool`, `int`, `float` and `string`.")
    []

let this_hint_outside_class pos =
  User_diagnostic.make_err
    Error_code.(to_enum ThisHintOutsideClass)
    (pos, "Cannot use `this` outside of a class")
    []

let parent_outside_class pos =
  User_diagnostic.make_err
    Error_codes.Typing.(to_enum ParentOutsideClass)
    (pos, "`parent` is undefined outside of a class")
    []

let self_outside_class pos =
  User_diagnostic.make_err
    Error_codes.Typing.(to_enum SelfOutsideClass)
    (pos, "`self` is undefined outside of a class")
    []

let static_outside_class pos =
  User_diagnostic.make_err
    Error_codes.Typing.(to_enum StaticOutsideClass)
    (pos, "`static` is undefined outside of a class")
    []

let this_type_forbidden pos in_extends in_req_extends =
  let msg =
    if in_extends then
      "This type `this` cannot be used in an `extends` clause"
    else if in_req_extends then
      "This type `this` cannot be used in a `require extends` clause"
    else
      "The type `this` cannot be used as a constraint on a class generic"
  in
  User_diagnostic.make_err Error_code.(to_enum ThisMustBeReturn) (pos, msg) []

let toplevel_statement pos =
  let msg =
    "Hack does not support top level statements. Use the `__EntryPoint` attribute on a function instead"
  in
  User_diagnostic.make_err Error_code.(to_enum ToplevelStatement) (pos, msg) []

let attribute_outside_allowed_files pos =
  User_diagnostic.make_err
    Error_code.(to_enum AttributeOutsideAllowedFiles)
    ( pos,
      "This attribute exists in an unapproved file. "
      ^ "The set of approved files is in .hhconfig. Please talk to someone from the Hack team before changing this."
    )
    []

let nonstatic_property_with_lsb pos =
  User_diagnostic.make_err
    Error_code.(to_enum NonstaticPropertyWithLSB)
    (pos, "`__LSB` attribute may only be used on static properties")
    []

let classname_param pos =
  User_diagnostic.make_err
    Error_code.(to_enum ClassnameParam)
    ( pos,
      "Missing type parameter to `classname`; `classname` is entirely"
      ^ " meaningless without one" )
    []

let tparam_applied_to_type pos tparam_name =
  User_diagnostic.make_err
    Error_code.(to_enum HigherKindedTypesUnsupportedFeature)
    ( pos,
      Printf.sprintf
        "`%s` is a type parameter. Type parameters cannot take type arguments (e.g. `%s<int>` isn't allowed)"
        tparam_name
        tparam_name )
    []

let shadowed_tparam prev_pos tparam_name pos =
  User_diagnostic.make_err
    Error_code.(to_enum ShadowedTypeParam)
    ( pos,
      Printf.sprintf
        "You cannot re-bind the type parameter %s"
        (Markdown_lite.md_codify tparam_name) )
    [
      ( Pos_or_decl.of_raw_pos prev_pos,
        Printf.sprintf
          "%s is already bound here"
          (Markdown_lite.md_codify tparam_name) );
    ]

let missing_typehint pos =
  User_diagnostic.make_err
    Error_code.(to_enum MissingTypehint)
    (pos, "Please add a type hint")
    []

let expected_variable pos =
  User_diagnostic.make_err
    Error_code.(to_enum ExpectedVariable)
    (pos, "Was expecting a variable name")
    []

let too_many_arguments pos =
  User_diagnostic.make_err
    Error_code.(to_enum NamingTooManyArguments)
    (pos, "Too many arguments")
    []

let too_few_arguments pos =
  User_diagnostic.make_err
    Error_code.(to_enum NamingTooFewArguments)
    (pos, "Too few arguments")
    []

let expected_collection pos cname =
  User_diagnostic.make_err
    Error_code.(to_enum ExpectedCollection)
    ( pos,
      Format.sprintf
        "Unexpected collection type %s"
        (Render.strip_ns cname |> Markdown_lite.md_codify) )
    []

let illegal_CLASS pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalClass)
    (pos, "Using `__CLASS__` outside a class or trait")
    []

let illegal_TRAIT pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalTrait)
    (pos, "Using `__TRAIT__` outside a trait")
    []

let illegal_member_variable_class pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalMemberVariableClass)
    ( pos,
      "Cannot declare a constant named `class`. The name `class` is reserved for the class constant that represents the name of the class"
    )
    []

let illegal_meth_caller pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalMethCaller)
    ( pos,
      "The two arguments to `meth_caller()` must be:"
      ^ "\n - first: `ClassOrInterface::class`"
      ^ "\n - second: a single-quoted string literal containing the name"
      ^ " of a non-static method of that class" )
    []

let illegal_class_meth pos =
  User_diagnostic.make_err
    Error_code.(to_enum IllegalClassMeth)
    ( pos,
      "The two arguments to `class_meth()` must be:"
      ^ "\n - first: `ValidClassname::class`"
      ^ "\n - second: a single-quoted string literal containing the name"
      ^ " of a static method of that class" )
    []

let illegal_constant pos = illegal_constant pos

let dynamic_method_access pos =
  User_diagnostic.make_err
    Error_code.(to_enum DynamicMethodAccess)
    ( pos,
      "Dynamic method access is not allowed. Please use the method name directly, for example `::myMethodName()`"
    )
    []

let class_meth_non_final_self pos class_name =
  User_diagnostic.make_err
    Error_code.(to_enum ClassMethNonFinalSelf)
    ( pos,
      Format.sprintf
        "`class_meth` with `self::class` does not preserve class calling context.\nUse `static::class`, or `%s::class` explicitly"
      @@ Render.strip_ns class_name )
    []

let class_meth_non_final_CLASS pos class_name is_trait =
  User_diagnostic.make_err
    Error_code.(to_enum ClassMethNonFinalCLASS)
    ( pos,
      if is_trait then
        "`class_meth` with `__CLASS__` in non-final classes is not allowed.\n"
      else
        Format.sprintf
          "`class_meth` with `__CLASS__` in non-final classes is not allowed.\nUse `%s::class` explicitly"
        @@ Render.strip_ns class_name )
    []

let self_in_non_final_function_pointer pos class_name meth_name =
  let suggestion =
    match class_name with
    | None -> ""
    | Some id ->
      let name =
        Markdown_lite.md_codify (Render.strip_ns id ^ "::" ^ meth_name)
      in
      "Consider using " ^ name ^ " instead"
  in
  User_diagnostic.make_err
    Error_code.(to_enum SelfInNonFinalFunctionPointer)
    ( pos,
      "Cannot use `self::` in a function pointer in a non-final class due to class context ambiguity. "
      ^ suggestion )
    []

let invalid_wildcard_context pos =
  User_diagnostic.make_err
    Error_code.(to_enum InvalidWildcardContext)
    ( pos,
      "A wildcard can only be used as a context when it is the sole context of a callable parameter in a higher-order function. The parameter must also be referenced with `ctx` in the higher-order function's context list, e.g. `function hof((function ()[_]: void) $f)[ctx $f]: void {}`"
    )
    []

let return_only_typehint pos kind =
  let kstr =
    Markdown_lite.md_codify
    @@
    match kind with
    | Hvoid -> "void"
    | Hnoreturn -> "noreturn"
  in
  User_diagnostic.make_err
    Error_code.(to_enum ReturnOnlyTypehint)
    ( pos,
      Format.sprintf
        "You can only use the %s type as the return type of a function or method"
        kstr )
    []

let unexpected_type_arguments pos =
  User_diagnostic.make_err
    Error_code.(to_enum UnexpectedTypeArguments)
    (pos, "Type arguments are not expected for this type")
    []

let too_many_type_arguments pos =
  User_diagnostic.make_err
    Error_code.(to_enum TooManyTypeArguments)
    (pos, "Too many type arguments for this type")
    []

let this_as_lexical_variable pos =
  User_diagnostic.make_err
    Error_code.(to_enum ThisAsLexicalVariable)
    (pos, "Cannot use `$this` as lexical variable")
    []

let explicit_consistent_constructor ck pos =
  let classish_kind =
    match ck with
    | Ast_defs.Cclass _ -> "class "
    | Ast_defs.Ctrait -> "trait "
    | Ast_defs.Cinterface -> "interface "
    | Ast_defs.Cenum_class _ -> "enum class "
    | Ast_defs.Cenum -> "enum "
  in
  User_diagnostic.make_err
    Error_code.(to_enum ExplicitConsistentConstructor)
    ( pos,
      "This "
      ^ classish_kind
      ^ "is marked <<__ConsistentConstruct>>, so it must declare a constructor explicitly"
    )
    []

let module_declaration_outside_allowed_files pos =
  User_diagnostic.make_err
    Error_code.(to_enum ModuleDeclarationOutsideAllowedFiles)
    ( pos,
      "This module declaration exists in an unapproved file. "
      ^ "The set of approved files is in .hhconfig" )
    []

let internal_module_level_trait pos =
  User_diagnostic.make_err
    Error_code.(to_enum InternalModuleLevelTrait)
    (pos, "Internal modules cannot have the <<__ModuleLevelTrait>> attribute")
    []

let deprecated_use pos fn_name =
  let msg =
    "The builtin "
    ^ Markdown_lite.md_codify (Render.strip_ns fn_name)
    ^ " is deprecated."
  in
  User_diagnostic.make_err
    Error_codes.Typing.(to_enum DeprecatedUse)
    (pos, msg)
    []

let unnecessary_attribute pos ~attr ~class_pos ~class_name ~suggestion =
  let class_name = Render.strip_ns class_name in
  let (reason_pos, reason_msg) =
    (class_pos, sprintf "the class `%s` is final" class_name)
  in
  let reason =
    let suggestion =
      match suggestion with
      | None -> "Try deleting this attribute"
      | Some s -> s
    in
    [
      ( Pos_or_decl.of_raw_pos reason_pos,
        "It is unnecessary because " ^ reason_msg );
      (Pos_or_decl.of_raw_pos pos, suggestion);
    ]
  in
  User_diagnostic.make_err
    Error_codes.Typing.(to_enum UnnecessaryAttribute)
    (pos, sprintf "The attribute `%s` is unnecessary" @@ Render.strip_ns attr)
    reason

let polymorphic_lambda_missing_return_hint pos =
  User_diagnostic.make_err
    Error_codes.Naming.(to_enum AddATypehint)
    ( pos,
      "Return hints must be given explicitly for polymorphic lambda expressions. Please add a type hint."
    )
    []

let polymorphic_lambda_missing_param_hint pos param_name =
  User_diagnostic.make_err
    Error_codes.Naming.(to_enum AddATypehint)
    ( pos,
      Format.sprintf
        {|Parameter hints must be given explicitly for polymorphic lambda expressions. Please add a type hint for `%s`.|}
        param_name )
    []

let render_custom_error
    (t : (string, Custom_error_eval.Value.t) Base.Either.t list) =
  List.fold_right
    t
    ~f:(fun v acc ->
      match v with
      | Core.Either.First str -> str ^ acc
      | Either.Second (Custom_error_eval.Value.Name nm) ->
        Markdown_lite.md_codify nm ^ acc
      | Either.Second _ -> acc)
    ~init:""

let to_user_diagnostic t custom_err_config =
  let custom_msgs =
    List.map ~f:render_custom_error
    @@ Custom_error_eval.eval_naming_error custom_err_config ~err:t
  in
  let f =
    match t with
    | Unexpected_arrow { pos; cname } -> unexpected_arrow pos cname
    | Missing_arrow { pos; cname } -> missing_arrow pos cname
    | Disallowed_xhp_type { pos; ty_name } -> disallowed_xhp_type pos ty_name
    | Name_is_reserved { pos; name } -> name_is_reserved pos name
    | Dollardollar_unused pos -> dollardollar_unused pos
    | Already_bound { pos; name } -> already_bound pos name
    | Method_name_already_bound { pos; meth_name } ->
      method_name_already_bound pos meth_name
    | Error_name_already_bound { pos; name; prev_pos } ->
      error_name_already_bound pos name prev_pos
    | Invalid_fun_pointer { pos; name } -> invalid_fun_pointer pos name
    | Undefined { pos; var_name; did_you_mean } ->
      undefined pos var_name did_you_mean
    | This_reserved pos -> this_reserved pos
    | Start_with_T pos -> start_with_T pos
    | Unexpected_typedef { pos; expected_kind; decl_pos } ->
      unexpected_typedef pos expected_kind decl_pos
    | Field_name_already_bound pos -> field_name_already_bound pos
    | Named_variadic_type_disallowed pos -> named_variadic_type_disallowed pos
    | Variadic_named_parameter_disallowed pos ->
      variadic_named_parameter_disallowed pos
    | Trait_self_shape_key { pos; key_name } ->
      trait_self_shape_key pos key_name
    | Primitive_top_level pos -> primitive_top_level pos
    | Primitive_invalid_alias { pos; ty_name_used; ty_name_canon } ->
      primitive_invalid_alias pos ty_name_used ty_name_canon
    | Dynamic_new_in_strict_mode pos -> dynamic_new_in_strict_mode pos
    | Invalid_type_access_root { pos; id } -> invalid_type_access_root pos id
    | Invalid_type_access_in_where pos -> invalid_type_access_in_where pos
    | Duplicate_user_attribute { attr_name; prev_pos; pos } ->
      duplicate_user_attribute attr_name prev_pos pos
    | Invalid_memoize_label { attr_name; pos } ->
      invalid_memoize_label attr_name pos
    | Unbound_name { pos; name; kind } -> unbound_name pos name kind
    | Unbound_attribute_name { pos; attr_name; closest_attr_name } ->
      unbound_attribute_name pos attr_name closest_attr_name
    | This_no_argument pos -> this_no_argument pos
    | Object_cast pos -> object_cast pos
    | This_hint_outside_class pos -> this_hint_outside_class pos
    | Parent_outside_class pos -> parent_outside_class pos
    | Self_outside_class pos -> self_outside_class pos
    | Static_outside_class pos -> static_outside_class pos
    | This_type_forbidden { pos; in_extends; in_req_extends } ->
      this_type_forbidden pos in_extends in_req_extends
    | Nonstatic_property_with_lsb pos -> nonstatic_property_with_lsb pos
    | Classname_param pos -> classname_param pos
    | Tparam_applied_to_type { pos; tparam_name } ->
      tparam_applied_to_type pos tparam_name
    | Shadowed_tparam { prev_pos; tparam_name; pos } ->
      shadowed_tparam prev_pos tparam_name pos
    | Missing_typehint pos -> missing_typehint pos
    | Expected_variable pos -> expected_variable pos
    | Too_many_arguments pos -> too_many_arguments pos
    | Too_few_arguments pos -> too_few_arguments pos
    | Expected_collection { pos; cname } -> expected_collection pos cname
    | Illegal_CLASS pos -> illegal_CLASS pos
    | Illegal_TRAIT pos -> illegal_TRAIT pos
    | Illegal_member_variable_class pos -> illegal_member_variable_class pos
    | Illegal_meth_caller pos -> illegal_meth_caller pos
    | Illegal_class_meth pos -> illegal_class_meth pos
    | Illegal_constant pos -> illegal_constant pos
    | Class_meth_non_final_self { pos; class_name } ->
      class_meth_non_final_self pos class_name
    | Class_meth_non_final_CLASS { pos; class_name; is_trait } ->
      class_meth_non_final_CLASS pos class_name is_trait
    | Const_without_typehint { pos; const_name; ty_name } ->
      const_without_typehint pos const_name ty_name
    | Prop_without_typehint { pos; prop_name; vis } ->
      prop_without_typehint pos prop_name vis
    | Invalid_require_implements pos -> invalid_req_implements pos
    | Invalid_require_extends pos -> invalid_req_extends pos
    | Invalid_require_class pos -> invalid_req_class pos
    | Invalid_require_constraint pos -> invalid_req_constraint pos
    | Invalid_wildcard_context pos -> invalid_wildcard_context pos
    | Did_you_mean { name; suggest_pos; suggest_name; pos } ->
      did_you_mean_naming pos name suggest_pos suggest_name
    | Using_internal_class { pos; class_name } ->
      using_internal_class pos class_name
    | Too_few_type_arguments pos -> too_few_type_arguments pos
    | Dynamic_class_name_in_strict_mode pos ->
      dynamic_class_name_in_strict_mode pos
    | Xhp_optional_required_attr { pos; attr_name } ->
      xhp_optional_required_attr pos attr_name
    | Wildcard_hint_disallowed pos -> wildcard_hint_disallowed pos
    | Wildcard_tparam_disallowed pos -> wildcard_param_disallowed pos
    | Illegal_use_of_dynamically_callable { meth_pos; vis; attr_pos } ->
      illegal_use_of_dynamically_callable attr_pos meth_pos vis
    | Parent_in_function_pointer { pos; meth_name; parent_name } ->
      parent_in_function_pointer pos meth_name parent_name
    | Self_in_non_final_function_pointer { pos; class_name; meth_name } ->
      self_in_non_final_function_pointer pos class_name meth_name
    | Return_only_typehint { pos; kind } -> return_only_typehint pos kind
    | Unexpected_type_arguments pos -> unexpected_type_arguments pos
    | Too_many_type_arguments pos -> too_many_type_arguments pos
    | This_as_lexical_variable pos -> this_as_lexical_variable pos
    | Explicit_consistent_constructor { pos; classish_kind } ->
      explicit_consistent_constructor classish_kind pos
    | Module_declaration_outside_allowed_files pos ->
      module_declaration_outside_allowed_files pos
    | Internal_module_level_trait pos -> internal_module_level_trait pos
    | Dynamic_method_access pos -> dynamic_method_access pos
    | Deprecated_use { pos; fn_name } -> deprecated_use pos fn_name
    | Unnecessary_attribute { pos; attr; class_pos; class_name; suggestion } ->
      unnecessary_attribute pos ~attr ~class_pos ~class_name ~suggestion
    | Undefined_in_expr_tree { pos; var_name; dsl; did_you_mean } ->
      undefined_in_expr_tree pos var_name dsl did_you_mean
    | Dynamic_hint_disallowed pos -> dynamic_hint_disallowed pos
    | Toplevel_statement pos -> toplevel_statement pos
    | Attribute_outside_allowed_files pos -> attribute_outside_allowed_files pos
    | Polymorphic_lambda_missing_return_hint pos ->
      polymorphic_lambda_missing_return_hint pos
    | Polymorphic_lambda_missing_param_hint { param_pos; param_name } ->
      polymorphic_lambda_missing_param_hint param_pos param_name
  in
  let user_error = f Explanation.empty in
  User_diagnostic.{ user_error with custom_msgs }
