(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Core
module Codes = Error_codes.Warning
module SN = Naming_special_names

module type Warning = sig
  type t

  val claim : t -> string

  val code : t -> Codes.t

  val codes : Codes.t list

  val reasons : t -> Pos_or_decl.t Message.t list

  val quickfixes : t -> Pos.t Quickfix.t list
end

module type Lint = sig
  include Warning

  val lint_code : t -> int

  val lint_severity : t -> Lints_core.severity

  val lint_quickfix : t -> Typing_warning.quickfix option
end

module Sketchy_equality = struct
  type t = Typing_warning.Sketchy_equality.t

  let code = Codes.SketchyEquality

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Sketchy_equality.result; _ } =
    let b =
      Markdown_lite.md_codify
        (if result then
          "true"
        else
          "false")
    in
    Printf.sprintf "This expression is likely always %s" b

  let reasons
      {
        Typing_warning.Sketchy_equality.result = _;
        left = (lazy left);
        right = (lazy right);
        left_trail;
        right_trail;
      } =
    let typedef_trail_entry pos = (pos, "Typedef definition comes from here") in
    let left_trail = List.map left_trail ~f:typedef_trail_entry in
    let right_trail = List.map right_trail ~f:typedef_trail_entry in
    left @ left_trail @ right @ right_trail

  let quickfixes _ = []
end

module Is_as_always = struct
  type t = Typing_warning.Is_as_always.t

  let code { Typing_warning.Is_as_always.kind; _ } =
    match kind with
    | Typing_warning.Is_as_always.Is_is_always_true -> Codes.IsIsAlwaysTrue
    | Typing_warning.Is_as_always.Is_is_always_false -> Codes.IsIsAlwaysFalse
    | Typing_warning.Is_as_always.As_always_succeeds _ -> Codes.AsAlwaysSucceeds
    | Typing_warning.Is_as_always.As_always_fails _ -> Codes.AsAlwaysFails

  let codes =
    [
      Codes.IsIsAlwaysTrue;
      Codes.IsIsAlwaysFalse;
      Codes.AsAlwaysSucceeds;
      Codes.AsAlwaysFails;
    ]

  let lint_severity _ = Lints_core.Lint_warning

  let claim { Typing_warning.Is_as_always.kind; lhs_ty; rhs_ty } =
    (match kind with
    | Typing_warning.Is_as_always.Is_is_always_true ->
      Printf.sprintf
        "This `is` check is always `true`. The expression on the left has type %s which is a subtype of %s."
    | Typing_warning.Is_as_always.Is_is_always_false ->
      Printf.sprintf
        "This `is` check is always `false`. The expression on the left has type %s which shares no values with %s."
    | Typing_warning.Is_as_always.As_always_succeeds _ ->
      Printf.sprintf
        "This `as` assertion will always succeed and hence is redundant. The expression on the left has a type %s which is a subtype of %s."
    | Typing_warning.Is_as_always.As_always_fails { is_nullable } ->
      let consequence =
        if is_nullable then
          "a `null` value"
        else
          "an exception at runtime"
      in
      Printf.sprintf
        "This `as` assertion will always fail and lead to %s. The expression on the left has type %s which shares no values with %s."
        consequence)
      (Markdown_lite.md_codify lhs_ty)
      (Markdown_lite.md_codify rhs_ty)

  let reasons _ = []

  let lint_code { Typing_warning.Is_as_always.kind; _ } =
    match kind with
    | Typing_warning.Is_as_always.Is_is_always_true ->
      Lints_codes.Codes.is_always_true
    | Is_is_always_false -> Lints_codes.Codes.is_always_false
    | As_always_succeeds _ -> Lints_codes.Codes.as_always_succeeds
    | As_always_fails _ -> Lints_codes.Codes.as_always_fails

  let lint_quickfix { Typing_warning.Is_as_always.kind; _ } =
    match kind with
    | Typing_warning.Is_as_always.Is_is_always_true
    | Is_is_always_false
    | As_always_fails _ ->
      None
    | As_always_succeeds quickfix -> Some quickfix

  let quickfixes _ = []
end

module Null_coalesce_always = struct
  type t = Typing_warning.Null_coalesce_always.t

  let code = Codes.NullCoalesceAlways

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Null_coalesce_always.kind; _ } =
    let side =
      match kind with
      | Null_coalesce_always_null -> "right"
      | Null_coalesce_never_null -> "left"
    in
    Format.sprintf
      "This null coalesce will always evaluate to its %s-hand side."
      side

  let reasons { Typing_warning.Null_coalesce_always.kind; lhs_pos; lhs_ty; _ } =
    let sub =
      match kind with
      | Null_coalesce_always_null -> "is always"
      | Null_coalesce_never_null -> "can never be"
    in
    [
      ( lhs_pos,
        Format.sprintf
          "The expression on the left has type %s which %s `null`."
          (Markdown_lite.md_codify lhs_ty)
          sub );
    ]

  let quickfixes _t = []
end

module Non_disjoint_check = struct
  type t = Typing_warning.Non_disjoint_check.t

  let code = Codes.NonDisjointCheck

  let codes = [code]

  let code _ = code

  let lint_code _ = Lints_codes.Codes.invalid_disjointness_check

  let lint_severity _ = Lints_core.Lint_warning

  let claim { Typing_warning.Non_disjoint_check.name; ty1; ty2; dynamic } =
    Printf.sprintf
      "%s to '%s' will always return the same value, because type %s is disjoint from type %s."
      (if dynamic then
        "Non-dynamic calls"
      else
        "This call")
      name
      ty1
      ty2

  let reasons _ = []

  let quickfixes _ = []

  let lint_quickfix _ = None
end

module Cast_non_primitive = struct
  type t = Typing_warning.Cast_non_primitive.t

  let code = Codes.CastNonPrimitive

  let codes = [code]

  let code _ = code

  let lint_code _ = Lints_codes.Codes.cast_non_primitive

  let lint_severity _ = Lints_core.Lint_error

  let claim { Typing_warning.Cast_non_primitive.cast_hint; ty } =
    Printf.sprintf
      ("Casting %s to %s: casting a non-primitive type to a primitive rarely yields a useful value. "
      ^^ "Did you mean to extract a value from this object before casting it, or to do a null-check?"
      )
      ty
      cast_hint

  let reasons _ = []

  let quickfixes _ = []

  let lint_quickfix _ = None
end

module Truthiness_test = struct
  open Typing_warning.Truthiness_test

  type nonrec t = t

  let code = Codes.TruthinessTest

  let codes = [code]

  let code _ = code

  let lint_code { kind; _ } =
    match kind with
    | Invalid _ -> Lints_codes.Codes.invalid_truthiness_test
    | Sketchy _ -> Lints_codes.Codes.sketchy_truthiness_test

  let lint_severity _ = Lints_core.Lint_warning

  let claim { kind; ty; expr; not } =
    match kind with
    | Invalid { truthy } ->
      Printf.sprintf
        "Invalid condition: a value of type %s will always be %s"
        (Markdown_lite.md_codify ty)
        (if truthy then
          "truthy"
        else
          "falsy")
    | Sketchy sketchy ->
      Printf.sprintf
        "%sketchy condition: testing the %s of %s may not behave as expected.\n%s"
        (match expr with
        | None -> "S"
        | Some e ->
          Printf.sprintf
            "`%s%s` is a s"
            (if not then
              "!"
            else
              "")
            e)
        (if not then
          "falsiness"
        else
          "truthiness")
        ty
        (match sketchy with
        | String ->
          Printf.sprintf
            "The values `\"\"` and `\"0\"` are both considered falsy. To check for %semptiness, use `%sStr\\is_empty%s`."
            (if not then
              ""
            else
              "non-")
            (if not then
              ""
            else
              "!")
            (match expr with
            | None -> ""
            | Some e -> Printf.sprintf "(%s)" e)
        | Arraykey ->
          "The values `0`, `\"\"`, and `\"0\"` are all considered falsy. Test for them explicitly."
        | Stringish ->
          "The values `\"\"` and `\"0\"` are both considered falsy, but objects will be truthy even if their `__toString` returns `\"\"` or `\"0\"`.\n"
          ^ "To check for emptiness, convert to a string and use `Str\\is_empty`."
        | Xhp_child ->
          "The values `\"\"` and `\"0\"` are both considered falsy, but objects (including XHP elements) will be truthy even if their `__toString` returns `\"\"` or `\"0\"`."
        | Traversable ->
          "A value of this type may be truthy even when empty.\n"
          ^ "Hack collections and arrays are falsy when empty, but user-defined Traversables will always be truthy, even when empty.\n"
          ^ "If you would like to only allow containers which are falsy when empty, use the `Container` or `KeyedContainer` interfaces.")

  let reasons _ = []

  let quickfixes _ = []

  let lint_quickfix _ = None
end

module Equality_check = struct
  open Typing_warning.Equality_check

  type t = Typing_warning.Equality_check.t

  let code = Codes.EqualityCheck

  let codes = [code]

  let code _ = code

  let lint_code { kind; _ } =
    match kind with
    | Equality _
    | Switch ->
      Lints_codes.Codes.non_equatable_comparison
    | Contains
    | Contains_key ->
      Lints_codes.Codes.invalid_contains_check

  let lint_severity _ = Lints_core.Lint_warning

  let claim { kind; ty1; ty2 } =
    match kind with
    | Equality b ->
      Printf.sprintf
        "Invalid comparison: This expression will always return %s.\nA value of type %s can never be equal to a value of type %s"
        (string_of_bool b |> Markdown_lite.md_codify)
        (Markdown_lite.md_codify ty1)
        (Markdown_lite.md_codify ty2)
    | Switch ->
      Printf.sprintf
        "Invalid case: This case will never match.\nA value of type %s can never be equal to a value of type %s"
        (Markdown_lite.md_codify ty1)
        (Markdown_lite.md_codify ty2)
    | Contains ->
      Printf.sprintf
        "Invalid `C\\contains` check: This call will always return `false`.\nA `Traversable<%s>` cannot contain a value of type %s"
        ty1
        (Markdown_lite.md_codify ty2)
    | Contains_key ->
      Printf.sprintf
        "Invalid `C\\contains_key` check: This call will always return `false`.\nA `KeyedTraversable<%s, ...>` cannot contain a key of type %s"
        ty1
        (Markdown_lite.md_codify ty2)

  let reasons _ = []

  let quickfixes _ = []

  let lint_quickfix _ = None
end

module Duplicated_properties = struct
  open Typing_warning.Duplicate_properties

  type t = Typing_warning.Duplicate_properties.t

  let code = Codes.DuplicateProperties

  let codes = [code]

  let code _ = code

  let lint_code { initialized_with_constant; _ } =
    if initialized_with_constant then
      Lints_codes.Codes.duplicate_property_enum_init
    else
      Lints_codes.Codes.duplicate_property

  let lint_severity { initialized_with_constant; _ } =
    if initialized_with_constant then
      Lints_core.Lint_error
    else
      Lints_core.Lint_warning

  let rec prettify_class_list names =
    match names with
    | [] -> ""
    | [c] -> c
    | [c1; c2] -> c1 ^ " and " ^ c2
    | h :: t -> h ^ ", " ^ prettify_class_list t

  let claim { initialized_with_constant; class_name; prop_name; class_names } =
    if initialized_with_constant then
      "Property "
      ^ (Utils.strip_ns prop_name |> Markdown_lite.md_codify)
      ^ ", defined in "
      ^ prettify_class_list (List.map ~f:Utils.strip_ns class_names)
      ^ ", is inherited multiple times by class "
      ^ (Utils.strip_ns class_name |> Markdown_lite.md_codify)
      ^ " and one of its instances is initialised with a class or enum constant"
    else
      "Duplicated property "
      ^ (Utils.strip_ns prop_name |> Markdown_lite.md_codify)
      ^ " in "
      ^ (Utils.strip_ns class_name |> Markdown_lite.md_codify)
      ^ " (defined in "
      ^ prettify_class_list
          (List.map
             ~f:(fun n -> Utils.strip_ns n |> Markdown_lite.md_codify)
             class_names)
      ^ "): all instances will be aliased at runtime"

  let reasons _ = []

  let quickfixes _ = []

  let lint_quickfix _ = None
end

module Class_pointer_to_string = struct
  type t = Typing_warning.Class_pointer_to_string.t

  let code = Codes.ClassPointerToString

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Class_pointer_to_string.ty; _ } =
    "Using "
    ^ ty
    ^ " in this position will trigger an implicit runtime conversion to string. You may use "
    ^ Markdown_lite.md_codify "HH\\class_to_classname"
    ^ " to get the class name as a string."

  let reasons _ = []

  let quickfixes _ = []
end

module No_disjoint_union_check = struct
  type t = Typing_warning.No_disjoint_union_check.t

  let code = Codes.NoDisjointUnion

  let codes = [code]

  let code _ = code

  let claim _ = "Invalid type argument due to disjoint types in a union"

  let reasons Typing_warning.No_disjoint_union_check.{ disjuncts; tparam_pos } =
    let disjuncts = Lazy.force disjuncts in
    let disjunct_reasons =
      List.map disjuncts ~f:(fun (pos, ty) ->
          (pos, Markdown_lite.md_codify ty ^ " is one of the disjoint types"))
    in
    disjunct_reasons
    @ [
        ( tparam_pos,
          "This type parameter is marked with "
          ^ Markdown_lite.md_codify SN.UserAttributes.uaNoDisjointUnion
          ^ " to indicate that disjoint unions are errors" );
      ]

  let quickfixes _ = []
end

module Switch_redundancy = struct
  open Typing_warning.Switch_redundancy

  type t = Typing_warning.Switch_redundancy.t

  let code = Codes.SwitchRedundancy

  let codes = [code]

  let code _ = code

  let claim = function
    | SwitchHasRedundancy { redundancy_size; _ } ->
      "This switch statement contains redundant cases."
      ^ " There are "
      ^ string_of_int redundancy_size
      ^ " redundant cases."
    | DuplicatedCase { case; _ } ->
      "This switch case, "
      ^ Markdown_lite.md_codify (Lazy.force case)
      ^ ", occurred before."
      ^ " It will never be matched."
    | RedundantDefault ->
      "All cases are already covered."
      ^ " Redundant `default` cases prevent detection of future errors."

  let reasons = function
    | SwitchHasRedundancy { positions; _ } ->
      List.map (Lazy.force positions) ~f:(fun p ->
          (Pos_or_decl.of_raw_pos p, "The case is redundant."))
    | DuplicatedCase { first_occurrence; _ } ->
      [
        ( Pos_or_decl.of_raw_pos first_occurrence,
          "This is the original occurrence of the case." );
      ]
    | RedundantDefault -> []

  let quickfixes _ = []
end

module Static_call_on_trait = struct
  open Typing_warning.Static_call_on_trait

  type t = Typing_warning.Static_call_on_trait.t

  let code = Codes.StaticCallOnTrait

  let codes = [code]

  let code _ = code

  let claim { trait_name; meth_name; _ } =
    let trait_call =
      Printf.sprintf "%s::%s" (Utils.strip_ns trait_name) meth_name
      |> Markdown_lite.md_codify
    in
    Printf.sprintf "Static method call on a trait: %s" trait_call

  let reasons { trait_pos; trait_name; _ } =
    let trait_name = Utils.strip_ns trait_name in
    [
      ( trait_pos,
        Printf.sprintf "%s is defined here" (Markdown_lite.md_codify trait_name)
      );
    ]

  let quickfixes _ = []
end

module Static_property_override = struct
  open Typing_warning.Static_property_override

  type t = Typing_warning.Static_property_override.t

  let code = Codes.StaticPropertyOverride

  let codes = [code]

  let code _ = code

  let claim { prop_name; _ } =
    Printf.sprintf
      "Static property is overridden: %s"
      (Markdown_lite.md_codify prop_name)

  let reasons { child_prop_pos; _ } =
    [(child_prop_pos, "Overriding static property is here")]

  let quickfixes _ = []
end

module Readonly_property_override = struct
  open Typing_warning.Readonly_property_override

  type t = Typing_warning.Readonly_property_override.t

  let code = Codes.ReadonlyPropertyOverride

  let codes = [code]

  let code _ = code

  let claim { prop_name; _ } =
    Printf.sprintf
      "The readonly-ness of property %s is changed by this override"
      (Markdown_lite.md_codify prop_name)

  let reasons { child_prop_pos; _ } =
    [(child_prop_pos, "Overriding property is here")]

  let quickfixes _ = []
end

module String_to_class_pointer = struct
  open Typing_warning.String_to_class_pointer

  type t = Typing_warning.String_to_class_pointer.t

  let code = Codes.StringToClassPointer

  let codes = [code]

  let code _ = code

  let claim { cls_name; _ } =
    let name = Utils.strip_ns cls_name in
    Printf.sprintf
      "It is no longer allowed to use a `classname<%s>` in this position. Please use a `class<%s>` instead."
      name
      name

  let reasons { ty_pos; _ } = [(ty_pos, "Definition is here")]

  let quickfixes _ = []
end

module Call_needs_concrete = struct
  open Typing_warning.Call_needs_concrete

  type t = Typing_warning.Call_needs_concrete.t

  let code = Codes.CallNeedsConcrete

  let codes = [code]

  let code _ = code

  let claim { class_name; meth_name; via; _ } =
    let strip_ns_class_name = Utils.strip_ns class_name in
    let member =
      Markdown_lite.md_codify (strip_ns_class_name ^ "::" ^ meth_name)
    in
    match via with
    | `Id ->
      Printf.sprintf
        "Dangerous call to %s (a `<<__NeedsConcrete>>` method). It is expecting a concrete receiver but %s is not concrete."
        member
        (Markdown_lite.md_codify strip_ns_class_name)
    | `Static ->
      Printf.sprintf
        "Dangerous call to %s (a `<<__NeedsConcrete>>` method) via `static`. It requires `static` to refer to a concrete class but `static` may not be concrete."
        member
    | (`Self | `Parent) as via ->
      let via_str =
        match via with
        | `Self -> "`self`"
        | `Parent -> "`parent`"
      in
      Printf.sprintf
        "Dangerous call to %s (a `<<__NeedsConcrete>>` method) via %s. It requires `static` to refer to a concrete class, but %s sets `static` to a class that may not be concrete."
        member
        via_str
        via_str

  let reasons { decl_pos; _ } = [(decl_pos, "Declaration is here")]

  let quickfixes _ = []
end

module Expect_bool_for_condition = struct
  type t = Typing_warning.Expect_bool_for_condition.t

  let code = Codes.ExpectBoolForCondition

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Expect_bool_for_condition.ty } =
    "Only bool values can be used as a condition. Use a type-specific test instead. This is "
    ^ ty

  let reasons _ = []

  let quickfixes _ = []
end

module Redundant_nullsafe_operation = struct
  type t = Typing_warning.Redundant_nullsafe_operation.t

  open Typing_warning.Redundant_nullsafe_operation

  let code { kind; _ } =
    match kind with
    | Redundant_nullsafe_member_select -> Codes.RedundantNullsafeMemberSelect
    | Nullsafe_member_select_on_null -> Codes.NullsafeMemberSelectOnNull
    | Redundant_nullsafe_pipe -> Codes.RedundantNullsafePipe
    | Nullsafe_pipe_on_null -> Codes.NullsafePipeOnNull

  let codes =
    [
      Codes.RedundantNullsafeMemberSelect;
      Codes.NullsafeMemberSelectOnNull;
      Codes.RedundantNullsafePipe;
      Codes.NullsafePipeOnNull;
    ]

  let claim { kind; ty } =
    match kind with
    | Redundant_nullsafe_member_select ->
      Printf.sprintf
        "You are using the %s operator but this object is of type %s which cannot be null. You can use the %s operator instead."
        (Markdown_lite.md_codify "?->")
        ty
        (Markdown_lite.md_codify "->")
    | Nullsafe_member_select_on_null ->
      Printf.sprintf
        "You are using the %s operator but this value is always null. It's likely you did not intend this value to be always null."
        (Markdown_lite.md_codify "?->")
    | Redundant_nullsafe_pipe ->
      Printf.sprintf
        "You are using the %s operator but this expression is of type %s which cannot be null. You can use the %s operator instead."
        (Markdown_lite.md_codify "|?>")
        ty
        (Markdown_lite.md_codify "|>")
    | Nullsafe_pipe_on_null ->
      Printf.sprintf
        "You are using the %s operator but this expression is always null. It's likely you did not intend this value to be always null."
        (Markdown_lite.md_codify "|?>")

  let reasons _ = []

  let quickfixes _ = []
end

module Unbound_name_warning = struct
  type t = Typing_warning.Unbound_name_warning.t

  let code = Codes.UnboundNameWarning

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Unbound_name_warning.name; kind_str } =
    "Unbound name: " ^ Markdown_lite.md_codify (Render.strip_ns name) ^ kind_str

  let reasons _ = []

  let quickfixes _ = []
end

module Set_or_keyset_array_get = struct
  type t = Typing_warning.Set_or_keyset_array_get.t

  let code = Codes.SetOrKeysetArrayGet

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Set_or_keyset_array_get.kind; ty; _ } =
    let suggestion =
      match kind with
      | Typing_warning.Set_or_keyset_array_get.Keyset -> "`C\\contains_key`"
      | Typing_warning.Set_or_keyset_array_get.ConstSet ->
        "the `contains` method"
    in
    Printf.sprintf
      "Array access on type %s returns the key (if present) or throws an exception. Consider using %s to check for membership instead."
      (Markdown_lite.md_codify ty)
      suggestion

  let reasons _ = []

  let quickfixes
      {
        Typing_warning.Set_or_keyset_array_get.kind;
        full_expr_pos;
        replacement_text;
        _;
      } =
    match replacement_text with
    | None -> []
    | Some new_text ->
      let title =
        match kind with
        | Typing_warning.Set_or_keyset_array_get.Keyset ->
          "Replace with `C\\contains_key`"
        | Typing_warning.Set_or_keyset_array_get.ConstSet ->
          "Replace with `->contains`"
      in
      [Quickfix.make_eager_default_hint_style ~title ~new_text full_expr_pos]
end

module Sealed_not_subtype = struct
  type t = Typing_warning.Sealed_not_subtype.t

  let code = Codes.SealedNotSubtype

  let codes = [code]

  let code _ = code

  let claim
      {
        Typing_warning.Sealed_not_subtype.verb;
        parent_name;
        parent_method_name;
        child_name;
        child_kind;
      } =
    let parent_name = Utils.strip_ns parent_name in
    let child_name = Utils.strip_ns child_name in
    child_kind
    ^ " "
    ^ Markdown_lite.md_codify child_name
    ^ " in sealed allowlist for "
    ^ (match parent_method_name with
      | Some method_name ->
        "method " ^ Markdown_lite.md_codify method_name ^ " in "
      | None -> "")
    ^ Markdown_lite.md_codify parent_name
    ^ ", but does not "
    ^ verb
    ^ " "
    ^ Markdown_lite.md_codify parent_name

  let reasons _ = []

  let quickfixes _ = []
end

module Tany_found = struct
  type t = Typing_warning.Tany_found.t

  let code = Codes.TanyFound

  let codes = [code]

  let code _ = code

  let claim () =
    "This expression has an unresolved type due to a limitation of the typechecker. It is silently compatible with every type, so it can be passed to or returned from any function without triggering a type error, potentially hiding bugs. Please post in the support group for further guidance."

  let reasons _ = []

  let quickfixes _ = []
end

module Dynamic_call_warning = struct
  type t = Typing_warning.Dynamic_call.t

  let codes =
    [
      Codes.DynamicMethodCall;
      Codes.DynamicFunctionCall;
      Codes.DynamicArrayAccess;
      Codes.DynamicPropertyAccess;
    ]

  let code { Typing_warning.Dynamic_call.kind; _ } =
    match kind with
    | Typing_warning.Dynamic_call.Method_invocation -> Codes.DynamicMethodCall
    | Typing_warning.Dynamic_call.Function_call -> Codes.DynamicFunctionCall
    | Typing_warning.Dynamic_call.Array_index -> Codes.DynamicArrayAccess
    | Typing_warning.Dynamic_call.Array_append -> Codes.DynamicArrayAccess
    | Typing_warning.Dynamic_call.Property_access -> Codes.DynamicPropertyAccess

  let claim { Typing_warning.Dynamic_call.kind; actual_type; _ } =
    let ty_str = Markdown_lite.md_codify actual_type in
    match kind with
    | Typing_warning.Dynamic_call.Method_invocation ->
      Printf.sprintf "Method invoked on a value of type %s" ty_str
    | Typing_warning.Dynamic_call.Function_call ->
      Printf.sprintf "Value of type %s called as a function" ty_str
    | Typing_warning.Dynamic_call.Array_index ->
      Printf.sprintf "Indexing into a value of type %s" ty_str
    | Typing_warning.Dynamic_call.Array_append ->
      Printf.sprintf "Appending to a value of type %s" ty_str
    | Typing_warning.Dynamic_call.Property_access ->
      Printf.sprintf "Property accessed on a value of type %s" ty_str

  let reasons
      { Typing_warning.Dynamic_call.dynamic_reason_pos; dynamic_reason_msg; _ }
      =
    [(dynamic_reason_pos, dynamic_reason_msg)]

  let quickfixes _ = []
end

module Dynamic_return = struct
  type t = Typing_warning.Dynamic_return.t

  let code = Codes.DynamicReturn

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Dynamic_return.return_type; _ } =
    Printf.sprintf
      "Returning a value of type `dynamic` through a non-fully-enforced return type %s"
      (Markdown_lite.md_codify return_type)

  let reasons { Typing_warning.Dynamic_return.return_type_pos; _ } =
    [(return_type_pos, "This return type is not fully enforced at runtime")]

  let quickfixes _ = []
end

module Consistent_construct_abstract_final = struct
  type t = Typing_warning.Consistent_construct_abstract_final.t

  let code = Codes.ConsistentConstructAbstractFinal

  let codes = [code]

  let code _ = code

  let claim
      {
        Typing_warning.Consistent_construct_abstract_final.name;
        inherited_from;
      } =
    let name = Render.strip_ns name in
    match inherited_from with
    | Some ancestor ->
      "An `abstract final` class cannot be `__ConsistentConstruct`. Class "
      ^ Markdown_lite.md_codify name
      ^ " inherits `__ConsistentConstruct` from "
      ^ Markdown_lite.md_codify (Render.strip_ns ancestor)
      ^ "."
    | None ->
      "An `abstract final` class cannot be `__ConsistentConstruct`. "
      ^ Markdown_lite.md_codify name
      ^ " is `__ConsistentConstruct`."

  let reasons _ = []

  let quickfixes _ = []
end

module Sealed_not_override = struct
  type t = Typing_warning.Sealed_not_override.t

  let code = Codes.SealedNotOverride

  let codes = [code]

  let code _ = code

  let claim
      {
        Typing_warning.Sealed_not_override.method_name;
        method_def;
        allowlist_class_name;
        allowlist_class_kind;
      } =
    let method_name = Utils.strip_ns method_name in
    let method_def = Utils.strip_ns method_def in
    let allowlist_class_name = Utils.strip_ns allowlist_class_name in
    allowlist_class_kind
    ^ " "
    ^ Markdown_lite.md_codify allowlist_class_name
    ^ " in sealed allowlist for method "
    ^ Markdown_lite.md_codify method_name
    ^ " in "
    ^ Markdown_lite.md_codify method_def
    ^ ", but does not override method "
    ^ Markdown_lite.md_codify method_name

  let reasons _ = []

  let quickfixes _ = []
end

module Redundant_require_this_as = struct
  type t = Typing_warning.Redundant_require_this_as.t

  let code = Codes.RedundantRequireThisAs

  let codes = [code]

  let code _ = code

  let claim { Typing_warning.Redundant_require_this_as.trait_name; class_name }
      =
    let trait_name = Utils.strip_ns trait_name in
    let class_name = Utils.strip_ns class_name in
    "Trait "
    ^ Markdown_lite.md_codify trait_name
    ^ " has `require this as "
    ^ Markdown_lite.md_codify class_name
    ^ "` constraint but class "
    ^ Markdown_lite.md_codify class_name
    ^ " does not use this trait. "
    ^ "Replace `require this as` with `require extends`"

  let reasons _ = []

  let quickfixes _ = []
end

let module_of (type a x) (kind : (x, a) Typing_warning.kind) :
    (module Warning with type t = x) =
  match kind with
  | Typing_warning.Sketchy_equality -> (module Sketchy_equality)
  | Typing_warning.Is_as_always -> (module Is_as_always)
  | Typing_warning.Non_disjoint_check -> (module Non_disjoint_check)
  | Typing_warning.Cast_non_primitive -> (module Cast_non_primitive)
  | Typing_warning.Truthiness_test -> (module Truthiness_test)
  | Typing_warning.Equality_check -> (module Equality_check)
  | Typing_warning.Duplicate_properties -> (module Duplicated_properties)
  | Typing_warning.Class_pointer_to_string -> (module Class_pointer_to_string)
  | Typing_warning.No_disjoint_union_check -> (module No_disjoint_union_check)
  | Typing_warning.Switch_redundancy -> (module Switch_redundancy)
  | Typing_warning.Static_call_on_trait -> (module Static_call_on_trait)
  | Typing_warning.Static_property_override -> (module Static_property_override)
  | Typing_warning.Readonly_property_override ->
    (module Readonly_property_override)
  | Typing_warning.String_to_class_pointer -> (module String_to_class_pointer)
  | Typing_warning.Null_coalesce_always -> (module Null_coalesce_always)
  | Typing_warning.Call_needs_concrete -> (module Call_needs_concrete)
  | Typing_warning.Expect_bool_for_condition ->
    (module Expect_bool_for_condition)
  | Typing_warning.Redundant_nullsafe_operation ->
    (module Redundant_nullsafe_operation)
  | Typing_warning.Unbound_name_warning -> (module Unbound_name_warning)
  | Typing_warning.Set_or_keyset_array_get -> (module Set_or_keyset_array_get)
  | Typing_warning.Sealed_not_subtype -> (module Sealed_not_subtype)
  | Typing_warning.Tany_found -> (module Tany_found)
  | Typing_warning.Consistent_construct_abstract_final ->
    (module Consistent_construct_abstract_final)
  | Typing_warning.Dynamic_call -> (module Dynamic_call_warning)
  | Typing_warning.Dynamic_return -> (module Dynamic_return)
  | Typing_warning.Sealed_not_override -> (module Sealed_not_override)
  | Typing_warning.Redundant_require_this_as ->
    (module Redundant_require_this_as)

let module_of_migrated
    (type x) (kind : (x, Typing_warning.migrated) Typing_warning.kind) :
    (module Lint with type t = x) =
  match kind with
  | Typing_warning.Is_as_always -> (module Is_as_always)
  | Typing_warning.Non_disjoint_check -> (module Non_disjoint_check)
  | Typing_warning.Cast_non_primitive -> (module Cast_non_primitive)
  | Typing_warning.Truthiness_test -> (module Truthiness_test)
  | Typing_warning.Equality_check -> (module Equality_check)
  | Typing_warning.Duplicate_properties -> (module Duplicated_properties)

(** A type-dependent warning is one that does depends on type precision and
    therefore can be invalidated by dynamic. Warnings which do not depend on
    type can always trusted. *)
let is_type_dependent (type a x) (kind : (x, a) Typing_warning.kind) : bool =
  match kind with
  (* Structural: don't depend on type information that could be invalidated by dynamic *)
  | Typing_warning.Static_call_on_trait -> false
  | Typing_warning.Static_property_override -> false
  | Typing_warning.Readonly_property_override -> false
  | Typing_warning.Duplicate_properties -> false
  | Typing_warning.Class_pointer_to_string -> false
  | Typing_warning.String_to_class_pointer -> false
  | Typing_warning.Call_needs_concrete -> false
  | Typing_warning.Unbound_name_warning -> false
  | Typing_warning.Sealed_not_subtype -> false
  | Typing_warning.Sealed_not_override -> false
  | Typing_warning.Consistent_construct_abstract_final -> false
  | Typing_warning.Redundant_require_this_as -> false
  (* Type-dependent: rely on type precision that could differ under dynamic *)
  | Typing_warning.Is_as_always -> true
  | Typing_warning.Non_disjoint_check -> true
  | Typing_warning.Cast_non_primitive -> true
  | Typing_warning.Truthiness_test -> true
  | Typing_warning.Equality_check -> true
  | Typing_warning.Sketchy_equality -> true
  | Typing_warning.Null_coalesce_always -> true
  | Typing_warning.Switch_redundancy -> true
  | Typing_warning.No_disjoint_union_check -> true
  | Typing_warning.Expect_bool_for_condition -> true
  | Typing_warning.Redundant_nullsafe_operation -> true
  | Typing_warning.Set_or_keyset_array_get -> true
  | Typing_warning.Dynamic_call -> true
  | Typing_warning.Dynamic_return -> true
  | Typing_warning.Tany_found -> true

let code_is_enabled tcopt code =
  match Typechecker_options.hack_warnings tcopt with
  | Global_options.NNone -> false
  | Global_options.All_except disabled_codes ->
    not (List.mem disabled_codes (Codes.to_enum code) ~equal:Int.equal)

let add_ tcopt (type a x) ((pos, kind, warning) : (x, a) Typing_warning.t) :
    unit =
  let (module M) = module_of kind in
  if code_is_enabled tcopt (M.code warning) then
    let severity =
      User_diagnostic.Warning { is_trusted = not (is_type_dependent kind) }
    in
    Diagnostics.add_diagnostic
      {
        User_diagnostic.severity;
        code = Codes.to_enum (M.code warning);
        claim = (pos, M.claim warning);
        reasons = M.reasons warning;
        explanation = Explanation.empty;
        quickfixes = M.quickfixes warning;
        custom_msgs = [];
        is_fixmed = false;
        function_pos = None;
      }

let add_for_migration
    tcopt
    (type x)
    ~(as_lint : Tast.check_status option option)
    (warning : (x, Typing_warning.migrated) Typing_warning.t) : unit =
  match as_lint with
  | None -> add_ tcopt warning
  | Some check_status ->
    let (pos, kind, warning) = warning in
    let (module M) = module_of_migrated kind in
    Lints_core.add
      ~autofix:
        (M.lint_quickfix warning |> Option.map ~f:Lints_diagnostics.quickfix)
      (M.lint_code warning)
      ~check_status
      (M.lint_severity warning)
      pos
      (M.claim warning)

let add env warning = add_ (Typing_env.get_tcopt env) warning

let codes (type a x) (kind : (x, a) Typing_warning.kind) :
    Error_codes.Warning.t list =
  let (module M) = module_of kind in
  M.codes
