(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

module Error_code = Error_codes.Typing

module Primary : sig
  module Shape : sig
    type t =
      | Invalid_shape_field_name of {
          pos: Pos.t;
          is_empty: bool;
        }
      | Invalid_shape_field_literal of {
          pos: Pos.t;
          witness_pos: Pos.t;
        }
      | Invalid_shape_field_const of {
          pos: Pos.t;
          witness_pos: Pos.t;
        }
      | Abstract_shape_field_const of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          name: string;
        }
      | Shape_field_class_const_missing_shape_key_attribute of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          class_name: string;
          const_name: string;
        }
      | Shape_key_const_value_unresolvable of {
          pos: Pos.t;
          reason_pos: Pos_or_decl.t;
          class_name: string;
          const_name: string;
          const_kind: [ `Enum | `ClassConst ];
        }
      | Shape_key_class_const_nameof_self_in_trait of {
          pos: Pos.t;
          value_pos: Pos.t;
          class_name: string;
          const_name: string;
        }
      | Shape_key_class_const_is_abstract of {
          pos: Pos.t;
          decl_pos: Pos.t;
          class_name: string;
          const_name: string;
        }
      | Invalid_shape_field_type of {
          pos: Pos.t;
          ty_pos: Pos_or_decl.t;
          ty_name: string Lazy.t;
          trail: Pos_or_decl.t list;
        }
      | Shape_field_class_mismatch of {
          pos: Pos.t;
          class_name: string;
          witness_pos: Pos.t;
          witness_class_name: string;
        }
      | Shape_field_type_mismatch of {
          pos: Pos.t;
          ty_name: string Lazy.t;
          witness_pos: Pos.t;
          witness_ty_name: string Lazy.t;
        }
      | Invalid_shape_remove_key of Pos.t
      | Shapes_key_exists_always_true of {
          pos: Pos.t;
          field_name: string;
          decl_pos: Pos_or_decl.t;
        }
      | Shapes_key_exists_always_false of {
          pos: Pos.t;
          field_name: string;
          decl_pos: Pos_or_decl.t;
          reason:
            [ `Nothing of Pos_or_decl.t Message.t list Lazy.t | `Undefined ];
        }
      | Shapes_method_access_with_non_existent_field of {
          pos: Pos.t;
          field_name: string;
          decl_pos: Pos_or_decl.t;
          method_name: string;
          reason:
            [ `Nothing of Pos_or_decl.t Message.t list Lazy.t | `Undefined ];
        }
      | Shapes_access_with_non_existent_field of {
          pos: Pos.t;
          field_name: string;
          decl_pos: Pos_or_decl.t;
          reason:
            [ `Nothing of Pos_or_decl.t Message.t list Lazy.t | `Undefined ];
        }
    [@@deriving show]
  end

  module Shape_and_tuple_destructure : sig
    type t =
      | Missing_ellipsis of Pos.t
      | Missing_required_fields of {
          pos: Pos.t;
          field_names: string;
        }
      | Unknown_field of {
          pos: Pos.t;
          field_name: string;
        }
      | Tuple_arity_mismatch of {
          pos: Pos.t;
          pat_arity: int;
          tuple_arity: int;
        }
      | Tuple_arity_exceeds of {
          pos: Pos.t;
          pat_arity: int;
          tuple_arity: int;
        }
      | Tuple_arity_union_conflict of Pos.t
    [@@deriving show]
  end

  module Switch : sig
    type t =
      | Switch_nonexhaustive of {
          switch_pos: Pos.t;
          missing: string list Lazy.t;
          scrutinee_pos: Pos.t;
          scrutinee_type: string Lazy.t;
        }
      | Switch_needs_default of {
          switch_pos: Pos.t;
          scrutinee_pos: Pos.t;
          scrutinee_type: string Lazy.t;
        }
    [@@deriving show]
  end

  module Enum : sig
    module Const : sig
      type t =
        | Null
        | Label of {
            class_: string;
            const: string;
          }
        | Bool of bool
        | Int of string option
        | String of string option
      [@@deriving eq, show, hash, sexp, ord]

      val to_user_string : t -> string

      val if_int : t -> then_:(string -> 'a) -> else_:'a -> 'a

      val opt_to_user_string : t option -> string
    end

    type t =
      | Enum_switch_redundant of {
          pos: Pos.t;
          first_pos: Pos.t;
          const_name: Const.t;
        }
      | Enum_switch_nonexhaustive of {
          pos: Pos.t;
          kind: string option;
          decl_pos: Pos_or_decl.t;
          missing: Const.t option list;
        }
      | Enum_switch_redundant_default of {
          pos: Pos.t;
          kind: string;
          decl_pos: Pos_or_decl.t;
        }
      | Enum_switch_not_const of Pos.t
      | Enum_switch_wrong_class of {
          pos: Pos.t;
          kind: string;
          expected: string lazy_t;
          actual: string lazy_t;
          expected_pos: Pos_or_decl.t option;
        }
      | Enum_switch_inconsistent_int_literal_format of {
          expected_pos: Pos.t;
          expected: string;
          actual: string;
          pos: Pos.t;
        }
      | Enum_type_bad of {
          pos: Pos.t;
          ty_name: string Lazy.t;
          is_enum_class: bool;
          trail: Pos_or_decl.t list;
        }
      | Enum_type_bad_case_type of {
          pos: Pos.t;
          ty_name: string Lazy.t;
          case_type_decl_pos: Pos_or_decl.t;
        }
      | Enum_constant_type_bad of {
          pos: Pos.t;
          ty_pos: Pos_or_decl.t;
          ty_name: string Lazy.t;
          trail: Pos_or_decl.t list;
        }
      | Enum_type_typedef_nonnull of Pos.t
      | Enum_class_label_as_expr of Pos.t
      | Enum_class_label_member_mismatch of {
          pos: Pos.t;
          label: string;
          expected_ty_msg_opt: Pos_or_decl.t Message.t list Lazy.t option;
        }
      | Incompatible_enum_inclusion_base of {
          pos: Pos.t;
          classish_name: string;
          src_classish_name: string;
        }
      | Incompatible_enum_inclusion_constraint of {
          pos: Pos.t;
          classish_name: string;
          src_classish_name: string;
        }
      | Enum_inclusion_not_enum of {
          pos: Pos.t;
          classish_name: string;
          src_classish_name: string;
        }
    [@@deriving show]
  end

  module Expr_tree : sig
    type t =
      | Expression_tree_non_public_member of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
        }
      | Reified_static_method_in_expr_tree of Pos.t
      | This_var_in_expr_tree of Pos.t
      | Experimental_expression_trees of Pos.t
      | Expression_tree_unsupported_operator of {
          pos: Pos.t;
          member_name: string;
          class_name: string;
        }
    [@@deriving show]
  end

  module Readonly : sig
    type t =
      | Readonly_modified of {
          pos: Pos.t;
          reason_opt: Pos_or_decl.t Message.t Lazy.t option;
        }
      | Readonly_mismatch of {
          pos: Pos.t;
          what: [ `arg_readonly | `arg_mut | `collection_mod | `prop_assign ];
          pos_sub: Pos_or_decl.t;
          pos_super: Pos_or_decl.t;
        }
      | Readonly_invalid_as_mut of Pos.t
      | Readonly_exception of Pos.t
      | Explicit_readonly_cast of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          kind: [ `fn_call | `property | `static_property ];
        }
      | Readonly_method_call of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
        }
      | Readonly_closure_call of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          suggestion: string;
        }
    [@@deriving show]
  end

  module Coeffect : sig
    type t =
      | Call_coeffect of {
          pos: Pos.t;
          available_pos: Pos_or_decl.t;
          available_incl_unsafe: string Lazy.t;
          required_pos: Pos_or_decl.t;
          required: string Lazy.t;
        }
      | Op_coeffect_error of {
          pos: Pos.t;
          op_name: string;
          locally_available: string Lazy.t;
          available_pos: Pos_or_decl.t;
          err_code: Error_code.t;
          required: string Lazy.t;
          suggestion: Pos_or_decl.t Message.t list Lazy.t option;
        }
    [@@deriving show]
  end

  module Wellformedness : sig
    type t =
      | Missing_return of {
          pos: Pos.t;
          hint_pos: Pos_or_decl.t option;
          is_async: bool;
        }
      | Void_usage of {
          pos: Pos.t;
          reason: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Noreturn_usage of {
          pos: Pos.t;
          reason: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Returns_with_and_without_value of {
          pos: Pos.t;
          with_value_pos: Pos.t;
          without_value_pos_opt: Pos.t option;
        }
      | Non_void_annotation_on_return_void_function of {
          is_async: bool;
          hint_pos: Pos.t;
        }
      | Tuple_syntax of Pos.t
      | Invalid_class_refinement of { pos: Pos.t }
      | Redundant_shape_splat of {
          pos: Pos.t;
          operand_pos: Pos.t;
          is_dynamic: bool;
        }
    [@@deriving show]
  end

  module Modules : sig
    type t =
      | Module_hint of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
        }
      | Module_mismatch of {
          pos: Pos.t;
          current_module_opt: string option;
          decl_pos: Pos_or_decl.t;
          target_module: string;
          tests_bypass_visibility_context: bool;
        }
      | Module_unsafe_trait_access of {
          access_pos: Pos.t;
          trait_pos: Pos_or_decl.t;
        }
    [@@deriving show]
  end

  module Package : sig
    type target_symbol_spec =
      | Func
      | Global_const
      | Class_const
      | Class
      | Enum
      | Symbol
      | Type_const
      | Reifiable_type_const of {
          reifiable_attr_pos: Pos_or_decl.t;
          tconst_name: string;
        }
      | Transparent_type_alias
      | Case_type
      | Reified_generic
      | Generic
      | As_expression
      | Enforceable_type_alias
      | Enforceable_enum
      | Attribute
      | Type_hint
    [@@deriving show]

    val target_symbol_spec_to_string : target_symbol_spec -> string

    type t =
      | Cross_pkg_access of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          current_package: Package.pos_id option;
          current_package_assignment_kind: string;
          target_package: Package.pos_id option;
          target_package_assignment_kind: string;
          current_filename: Relative_path.t;
          target_filename: Relative_path.t;
          target_id: string;
          target_symbol_spec: target_symbol_spec;
          loaded_packages: Package.pos_id list;
          included_packages: (string * string * Pos.t) list;
        }
      | Cross_pkg_access_with_requirepackage of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          target_package: string;
          current_package: Package.pos_id option;
          loaded_packages: Package.pos_id list;
          included_packages: (string * string * Pos.t) list;
        }
      | Cross_pkg_access_with_softrequirepackage of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          current_soft_package_opt: (Pos.t * string) option;
          target_package: string;
          loaded_packages: Package.pos_id list;
          included_packages: (string * string * Pos.t) list;
        }
      | Soft_included_access of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          current_package: Package.pos_id option;
          current_package_assignment_kind: string;
          target_package: Package.pos_id option;
          target_package_assignment_kind: string;
          current_filename: Relative_path.t;
          target_filename: Relative_path.t;
          target_id: string;
          target_symbol_spec: target_symbol_spec;
          loaded_packages: Package.pos_id list;
          included_packages: (string * string * Pos.t) list;
        }
      | Excluded_path_access of {
          pos: Pos.t;
          decl_pos: Pos_or_decl.t;
          package: Package.pos_id option;
          target_filename: Relative_path.t;
          target_id: string;
          target_symbol_spec: target_symbol_spec;
        }
    [@@deriving show]
  end

  module Xhp : sig
    type t =
      | Xhp_required of {
          pos: Pos.t;
          why_xhp: string;
          ty_reason_msg: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Illegal_xhp_child of {
          pos: Pos.t;
          ty_reason_msg: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Missing_xhp_required_attr of {
          pos: Pos.t;
          attr: string;
          ty_reason_msg: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Attribute_value of {
          pos: Pos.t;
          attr_name: string;
          valid_values: string list;
        }
    [@@deriving show]
  end

  module CaseType : sig
    type t =
      | Overlapping_variant_types of {
          pos: Pos.t;
          name: string;
          why: Pos_or_decl.t Message.t list Lazy.t;
        }
      | Invalid_recursive of {
          pos: Pos.t;
          name: string;
        }
      | Recursive_where_clause of {
          pos: Pos.t;
          name: string;
          mentions: Pos_or_decl.t list;
        }
    [@@deriving show]
  end

  module SimpliHack : sig
    type t =
      | Run_prompt of { pos: Pos.t }
      | Rerun_prompt of {
          pos: Pos.t;
          prompt_digest: string;
          expected_digest: string;
        }
      | Evaluation_error of {
          pos: Pos.t;
          stack_trace: Pos_or_decl.t Message.t list Lazy.t;
        }
    [@@deriving show]
  end

  module Shape_splat : sig
    type overlap_source_origin =
      | Shape_field of string
      | Description of string
    [@@deriving show]

    type overlap_source = {
      pos: Pos_or_decl.t;
      origin: overlap_source_origin;
    }
    [@@deriving show]

    (** All the ways in which two shape splat elements can be non-disjoint *)
    type disjointness_violation =
      | Overlapping_field of {
          label: string;
          positions: Pos_or_decl.t list;
        }
      | Possible_overlapping_field of {
          label: string;
          positions: Pos_or_decl.t list;
          sources: overlap_source list;
        }
      | Unresolved_sources of { sources: overlap_source list }
    [@@deriving show]

    type non_denotable_field = {
      label: string;
      pos: Pos_or_decl.t;
      ty: Typing_defs_core.locl_ty;
    }
    [@@deriving show]

    type t =
      | Disjoint_shape_splat_violation of {
          pos: Pos.t;
          violations: disjointness_violation list;
        }
      | Non_denotable_shape_splat_fields of {
          pos: Pos.t;
          fields: non_denotable_field list;
        }
    [@@deriving show]
  end

  type implements_info = {
    pos: Pos_or_decl.t;
    instantiation: string list;
    via_direct_parent: Pos.t * string;
  }

  type dynamic_obj_get_member = {
    member_pos: Pos.t;
    member_name_pos: Pos.t;
    member_name: string;
  }
  [@@deriving show]

  (** Specific error information readily transformable into a user error *)
  type t =
    (* == Factorised errors ================================================= *)
    | Coeffect of Coeffect.t
    | Enum of Enum.t
    | Expr_tree of Expr_tree.t
    | Modules of Modules.t
    | Package of Package.t
    | Readonly of Readonly.t
    | Shape of Shape.t
    | Shape_and_tuple_destructure of Shape_and_tuple_destructure.t
    | Switch of Switch.t
    | Wellformedness of Wellformedness.t
    | Xhp of Xhp.t
    | CaseType of CaseType.t
    | SimpliHack of SimpliHack.t
    (* == Primary only ====================================================== *)
    | Unresolved_tyvar of Pos.t
    | Unify_error of {
        pos: Pos.t;
        msg_opt: string option;
        reasons_opt: Pos_or_decl.t Message.t list Lazy.t option;
      }
    | Generic_unify of {
        pos: Pos.t;
        msg: string;
      }
    | Using_error of {
        pos: Pos.t;
        has_await: bool;
      }
    | Bad_enum_decl of Pos.t
    | Bad_conditional_support_dynamic of {
        pos: Pos.t;
        child: string;
        parent: string;
        ty_name: string Lazy.t;
        self_ty_name: string Lazy.t;
      }
    | Bad_decl_override of {
        pos: Pos.t;
        name: string;
        parent_name: string;
      }
    | Explain_where_constraint of {
        pos: Pos.t;
        in_class: bool;
        decl_pos: Pos_or_decl.t;
      }
    | Explain_constraint of Pos.t
    | Rigid_tvar_escape of {
        pos: Pos.t;
        what: string;
      }
    | Invalid_type_hint of Pos.t
    | Shape_splat of Shape_splat.t
    | Gated_by_feature_flag of {
        pos: Pos.t;
        name: string;
        feature: string;
      }
    | Tests_bypass_visibility_on_member_without_class of {
        pos: Pos.t;
        member_name: string;
        class_name: string;
      }
    | Unsatisfied_req of {
        pos: Pos.t;
        trait_pos: Pos_or_decl.t;
        req_pos: Pos_or_decl.t;
        req_name: string;
      }
    | Unsatisfied_req_class of {
        pos: Pos.t;
        trait_pos: Pos_or_decl.t;
        req_pos: Pos_or_decl.t;
        req_name: string;
      }
    | Unsatisfied_req_this_as of {
        pos: Pos.t;
        trait_pos: Pos_or_decl.t;
        req_pos: Pos_or_decl.t;
        req_name: string;
      }
    | Incompatible_reqs of {
        pos: Pos.t;
        req_name: string;
        req_class_pos: Pos_or_decl.t;
        req_extends_pos: Pos_or_decl.t;
      }
    | Trait_not_used of {
        pos: Pos.t;
        trait_name: string;
        req_class_pos: Pos_or_decl.t;
        class_pos: Pos_or_decl.t;
        class_name: string;
        req_constraint_kind: [ `class_ | `this_as ];
      }
    | Invalid_echo_argument of Pos.t
    | Index_type_mismatch of {
        pos: Pos.t;
        is_covariant_container: bool;
        msg_opt: string option;
        reasons_opt: Pos_or_decl.t Message.t list Lazy.t option;
      }
    | Member_not_found of {
        pos: Pos.t;
        kind: [ `method_ | `property ];
        class_name: string;
        class_pos: Pos_or_decl.t;
        member_name: string;
        hint: ([ `instance | `static ] * Pos_or_decl.t * string) option Lazy.t;
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Construct_not_instance_method of Pos.t
    | Ambiguous_inheritance of {
        pos: Pos.t;
        class_name: string;
        origin: string;
      }
    | Expected_tparam of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        n: int;
      }
    | Typeconst_concrete_concrete_override of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Constant_multiple_concrete_conflict of {
        pos: Pos.t;
        name: string;
        definitions: (Pos_or_decl.t * string option) list;
      }
    | Invalid_memoized_param of {
        pos: Pos.t;
        ty_name: string Lazy.t;
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Invalid_arraykey of {
        pos: Pos.t;
        ctxt: [ `read | `write ];
        container_pos: Pos_or_decl.t;
        container_ty_name: string Lazy.t;
        key_pos: Pos_or_decl.t;
        key_ty_name: string Lazy.t;
      }
    | Invalid_keyset_value of {
        pos: Pos.t;
        container_pos: Pos_or_decl.t;
        container_ty_name: string Lazy.t;
        value_pos: Pos_or_decl.t;
        value_ty_name: string Lazy.t;
      }
    | Invalid_set_value of {
        pos: Pos.t;
        container_pos: Pos_or_decl.t;
        container_ty_name: string Lazy.t;
        value_pos: Pos_or_decl.t;
        value_ty_name: string Lazy.t;
      }
    | Invalid_substring of {
        pos: Pos.t;
        ty_name: string Lazy.t;
      }
    | Unset_nonidx_in_strict of {
        pos: Pos.t;
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Nullable_cast of {
        pos: Pos.t;
        ty_pos: Pos_or_decl.t;
        ty_name: string Lazy.t;
      }
    | Hh_expect of {
        pos: Pos.t;
        equivalent: bool;
      }
    | Null_member of {
        pos: Pos.t;
        obj_pos_opt: Pos.t option;
        ctxt: [ `read | `write ];
        kind: [ `method_ | `property ];
        member_name: string;
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Nullsafe_property_write_context of Pos.t
    | Uninstantiable_class of {
        pos: Pos.t;
        class_name: string;
        reason_ty_opt: (Pos.t * string Lazy.t) option;
        decl_pos: Pos_or_decl.t;
      }
    | Abstract_const_usage of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        name: string;
      }
    | Member_not_implemented of {
        pos: Pos.t;
        member_name: string;
        decl_pos: Pos_or_decl.t;
        quickfixes: Pos.t Quickfix.t list;
      }
    | Kind_mismatch of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        tparam_name: string;
        expected_kind: string;
        actual_kind: string;
      }
    | Trait_parent_construct_inconsistent of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Top_member of {
        pos: Pos.t;
        ctxt: [ `read | `write ];
        ty_name: string Lazy.t;
        decl_pos: Pos_or_decl.t;
        kind: [ `method_ | `property ];
        name: string;
        is_nullable: bool;
        ty_reasons: (Pos_or_decl.t * string) list Lazy.t;
      }
    | Unresolved_tyvar_projection of {
        pos: Pos.t;
        proj_pos: Pos_or_decl.t;
        tconst_name: string;
      }
    | Cyclic_class_constant of {
        pos: Pos.t;
        class_name: string;
        const_name: string;
      }
    | Inout_annotation_missing of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Inout_annotation_unexpected of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        param_is_variadic: bool;
        qfx_pos: Pos.t;
      }
    | Inout_argument_bad_type of {
        pos: Pos.t;
        reasons: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Invalid_meth_caller_inout_parameter of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Invalid_meth_caller_named_parameter of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Invalid_meth_caller_readonly_return of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | This_as_function_pointer_param of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Invalid_new_disposable of Pos.t
    | Invalid_return_disposable of Pos.t
    | Invalid_disposable_hint of {
        pos: Pos.t;
        class_name: string;
      }
    | Invalid_disposable_return_hint of {
        pos: Pos.t;
        class_name: string;
      }
    | Ambiguous_lambda of {
        pos: Pos.t;
        uses: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Wrong_extend_kind of {
        pos: Pos.t;
        kind: Ast_defs.classish_kind;
        name: string;
        parent_pos: Pos_or_decl.t;
        parent_kind: Ast_defs.classish_kind;
        parent_name: string;
      }
    | Wrong_use_kind of {
        pos: Pos.t;
        name: string;
        parent_pos: Pos_or_decl.t;
        parent_name: string;
      }
    | Cyclic_class_def of {
        pos: Pos.t;
        stack: S_set.t;
      }
    | Trait_reuse_with_final_method of {
        pos: Pos.t;
        trait_name: string;
        parent_cls_name: string Lazy.t;
        trace: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Trait_reuse_inside_class of {
        pos: Pos.t;
        class_name: string;
        trait_name: string;
        occurrences: Pos_or_decl.t list;
      }
    | Invalid_is_as_expression_hint of {
        pos: Pos.t;
        op: [ `is | `as_ ];
        reasons: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Invalid_enforceable_type of {
        pos: Pos.t;
        ty_info: Pos_or_decl.t Message.t list Lazy.t;
        tp_pos: Pos_or_decl.t;
        tp_name: string;
        kind: [ `constant | `param ];
      }
    | Reifiable_attr of {
        pos: Pos.t;
        ty_info: Pos_or_decl.t Message.t list Lazy.t;
        attr_pos: Pos_or_decl.t;
        kind: [ `ty | `cnstr | `super_cnstr ];
      }
    | Invalid_newable_type_argument of {
        pos: Pos.t;
        tp_pos: Pos_or_decl.t;
        tp_name: string;
      }
    | Invalid_newable_typaram_constraints of {
        pos: Pos.t;
        tp_name: string;
        constraints: string list;
      }
    | Override_per_trait of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
        trait_name: string;
        meth_pos: Pos_or_decl.t;
      }
    | Should_not_be_override of {
        pos: Pos.t;
        class_id: string;
        id: string;
      }
    | Needs_concrete_in_final_class of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
      }
    | Needs_concrete_on_instance_method of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
      }
    | Needs_concrete_on_constructor of {
        pos: Pos.t;
        class_name: string;
      }
    | Call_needs_concrete of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
        decl_pos: Pos_or_decl.t;
        via: [ `Id | `Self | `Parent | `Static ];
      }
    | Abstract_access_via_static of {
        pos: Pos.t;
        class_name: string;
        member_name: string;
        decl_pos: Pos_or_decl.t;
        containing_method_pos: Pos.t option;
      }
    | Uninstantiable_class_via_static of {
        pos: Pos.t;
        class_name: string;
        decl_pos: Pos_or_decl.t;
        containing_method_pos: Pos.t option;
      }
    | This_refinement_in_generic_class of {
        pos: Pos.t;
        class_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Private_member_variance of {
        pos: Pos.t;
        class_name: string;
        member_name: string;
        is_write: bool;
        tparam_name: string;
        variance: Ast_defs.variance;
        use_pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Memoize_erased_generic_return of {
        pos: Pos.t;
        tparam_name: string;
        attr_pos: Pos.t;
        via_param: Pos.t option;
      }
    | Needs_concrete_override of {
        pos: Pos.t;
        method_pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
        method_name_for_method_defined_outside_class: string option;
      }
    | Trivial_strict_eq of {
        pos: Pos.t;
        result: bool;
        left: Pos_or_decl.t Message.t list Lazy.t;
        right: Pos_or_decl.t Message.t list Lazy.t;
        left_trail: Pos_or_decl.t list;
        right_trail: Pos_or_decl.t list;
      }
    | Trivial_strict_not_nullable_compare_null of {
        pos: Pos.t;
        result: bool;
        ty_reason_msg: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Eq_incompatible_types of {
        pos: Pos.t;
        left: Pos_or_decl.t Message.t list Lazy.t;
        right: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Comparison_invalid_types of {
        pos: Pos.t;
        left: Pos_or_decl.t Message.t list Lazy.t;
        right: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Strict_eq_value_incompatible_types of {
        pos: Pos.t;
        left: Pos_or_decl.t Message.t list Lazy.t;
        right: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Deprecated_use of {
        pos: Pos.t;
        decl_pos_opt: Pos_or_decl.t option;
        msg: string;
      }
    | Cannot_declare_constant of {
        pos: Pos.t;
        class_pos: Pos.t;
        class_name: string;
      }
    | Redeclaring_promoted_property of {
        pos: Pos.t;
        prop_name: string;
        parent_pos: Pos_or_decl.t;
        parent_name: string;
      }
    | Invalid_classname of Pos.t
    | Illegal_type_structure of {
        pos: Pos.t;
        msg: string;
        fn: string;
      }
    | Illegal_typeconst_direct_access of Pos.t
    | Wrong_expression_kind_attribute of {
        pos: Pos.t;
        attr_name: string;
        expr_kind: string;
        attr_class_pos: Pos_or_decl.t;
        attr_class_name: string;
        intf_name: string;
      }
    | Ambiguous_object_access of {
        pos: Pos.t;
        name: string;
        self_pos: Pos_or_decl.t;
        vis: string;
        subclass_pos: Pos_or_decl.t;
        class_self: string;
        class_subclass: string;
      }
    | Invalid_arraykey_constraint of {
        pos: Pos.t;
        ty_name: string Lazy.t;
      }
    | Redundant_generic of {
        pos: Pos.t;
        variance: [ `Co | `Contra ];
        msg: string;
        suggest: string;
      }
    | Meth_caller_trait of {
        pos: Pos.t;
        trait_name: string;
      }
    | Duplicate_interface of {
        pos: Pos.t;
        name: string;
        others: Pos_or_decl.t list;
      }
    | Enum_duplicate_value of {
        pos: Pos.t;  (** the enum definition *)
        value: string;
        member_name: string;
        member_pos: Pos_or_decl.t;
        prev_name: string;
        prev_pos: Pos_or_decl.t;
      }
    | Enum_uncheckable_value of {
        pos: Pos.t;  (** the enum definition *)
        name: string;  (** the enum's name *)
        reasons: ((Pos_or_decl.t * string) list * int) Lazy.t;
            (** Lazily: [(reasons, num_omitted)] -- why the enum can't be
                statically checked for duplicate values (capped for display) and
                how many further reasons were omitted. Lazy so the reason strings
                are only built when the error is rendered. *)
      }
    | Reified_function_reference of Pos.t
    | Reinheriting_classish_const of {
        pos: Pos.t;
        classish_name: string;
        src_pos: Pos.t;
        src_classish_name: string;
        existing_const_origin: string;
        const_name: string;
      }
    | Redeclaring_classish_const of {
        pos: Pos.t;
        classish_name: string;
        redeclaration_pos: Pos.t;
        existing_const_origin: string;
        const_name: string;
      }
    | Abstract_function_pointer of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Inherited_class_member_with_different_case of {
        pos: Pos.t;
        member_type: string;
        name: string;
        name_prev: string;
        child_class: string;
        prev_class: string;
        prev_class_pos: Pos_or_decl.t;
      }
    | Multiple_inherited_class_member_with_different_case of {
        pos: Pos.t;
        child_class_name: string;
        member_type: string;
        class1_name: string;
        class1_pos: Pos_or_decl.t;
        name1: string;
        class2_name: string;
        class2_pos: Pos_or_decl.t;
        name2: string;
      }
    | Multiple_instantiation_inheritence of {
        type_name: string;
        implements_or_extends: string;
        interface_name: string;
        winning_implements: implements_info;
        losing_implements: implements_info;
      }
    | Parent_support_dynamic_type of {
        pos: Pos.t;
        child_name: string;
        child_kind: Ast_defs.classish_kind;
        parent_name: string;
        parent_kind: Ast_defs.classish_kind;
        child_support_dyn: bool;
      }
    | Property_is_not_enforceable of {
        pos: Pos.t;
        prop_name: string;
        class_name: string;
        prop_pos: Pos_or_decl.t;
        prop_type: string;
      }
    | Property_is_not_dynamic of {
        pos: Pos.t;
        prop_name: string;
        class_name: string;
        prop_pos: Pos_or_decl.t;
        prop_type: string;
      }
    | Private_property_is_not_enforceable of {
        pos: Pos.t;
        prop_name: string;
        class_name: string;
        prop_pos: Pos_or_decl.t;
        prop_type: string;
      }
    | Private_property_is_not_dynamic of {
        pos: Pos.t;
        prop_name: string;
        class_name: string;
        prop_pos: Pos_or_decl.t;
        prop_type: string;
      }
    | Immutable_local of Pos.t
    | Require_dynamic_obj_get of {
        receiver_pos: Pos.t;
        receiver_ty: string Lazy.t;
        receiver_ty_pos: Pos_or_decl.t;
        member_kind: [ `Property | `Method ];
        member_name: dynamic_obj_get_member option;
      }
    | Nonsense_member_selection of {
        pos: Pos.t;
        kind: string;
      }
    | Consider_meth_caller of {
        pos: Pos.t;
        class_name: string;
        meth_name: string;
      }
    | Method_import_via_diamond of {
        pos: Pos.t;
        class_name: string;
        method_pos: Pos_or_decl.t;
        method_name: string;
        trace1: Pos_or_decl.t Message.t list Lazy.t;
        trace2: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Property_import_via_diamond of {
        generic: bool;
        pos: Pos.t;
        class_name: string;
        property_pos: Pos_or_decl.t;
        property_name: string;
        trace1: Pos_or_decl.t Message.t list Lazy.t;
        trace2: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Unification_cycle of {
        pos: Pos.t;
        ty_name: string Lazy.t;
      }
    | Method_variance of Pos.t
    | Explain_tconst_where_constraint of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        msgs: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Format_string of {
        pos: Pos.t;
        snippet: string;
        fmt_string: string;
        class_pos: Pos_or_decl.t;
        fn_name: string;
        class_suggest: string;
      }
    | Expected_literal_format_string of Pos.t
    | Re_prefixed_non_string of {
        pos: Pos.t;
        reason: [ `non_string | `embedded_expr ];
      }
    | Bad_regex_pattern of {
        pos: Pos.t;
        reason:
          [ `missing_delim
          | `empty_patt
          | `invalid_option
          | `bad_patt of string
          ];
      }
    | Option_return_only_typehint of {
        pos: Pos.t;
        kind: [ `void | `noreturn ];
      }
    | Redeclaring_missing_method of {
        pos: Pos.t;
        trait_method: string;
      }
    | Expecting_type_hint of Pos.t
    | Expecting_type_hint_variadic of Pos.t
    | Expecting_return_type_hint of Pos.t
    | Duplicate_using_var of Pos.t
    | Illegal_disposable of {
        pos: Pos.t;
        verb: [ `assigned ];
      }
    | Escaping_disposable of Pos.t
    | Escaping_disposable_param of Pos.t
    | Escaping_this of Pos.t
    | Must_extend_disposable of Pos.t
    | Field_kinds of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Unbound_name of {
        pos: Pos.t;
        name: string;
        class_exists: bool;
      }
    | Previous_default of Pos.t
    | Previous_default_or_optional of Pos.t
    | Return_in_void of {
        pos: Pos.t;
        decl_pos: Pos.t;
      }
    | This_var_outside_class of Pos.t
    | Unbound_global of Pos.t
    | Private_meth_caller of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Protected_meth_caller of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Internal_meth_caller of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Protected_internal_meth_caller of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Array_cast of Pos.t
    | String_cast of {
        pos: Pos.t;
        ty_name: string Lazy.t;
      }
    | Static_outside_class of Pos.t
    | Self_outside_class of Pos.t
    | New_inconsistent_construct of {
        pos: Pos.t;
        class_pos: Pos_or_decl.t;
        class_name: string;
        kind: [ `static | `classname ];
      }
    | Parent_outside_class of Pos.t
    | Parent_abstract_call of {
        pos: Pos.t;
        meth_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Self_abstract_call of {
        pos: Pos.t;
        self_pos: Pos.t;
        meth_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Classname_abstract_call of {
        pos: Pos.t;
        meth_name: string;
        class_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Static_synthetic_method of {
        pos: Pos.t;
        meth_name: string;
        class_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Static_call_on_trait_require_non_strict of {
        pos: Pos.t;
        meth_name: string;
        trait_name: string;
        req_constraint_name: string;
        req_constraint_kind: [ `class_ | `this_as ];
        trait_pos: Pos_or_decl.t;
      }
    | Static_prop_on_trait of {
        pos: Pos.t;
        meth_name: string;
        trait_name: string;
        trait_pos: Pos_or_decl.t;
      }
    | Isset_in_strict of Pos.t
    | Isset_inout_arg of Pos.t
    | Unpacking_disallowed_builtin_function of {
        pos: Pos.t;
        fn_name: string;
      }
    | Array_get_arity of {
        pos: Pos.t;
        name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Undefined_field of {
        pos: Pos.t;
        name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Array_access of {
        pos: Pos.t;
        ctxt: [ `read | `write ];
        ty_name: string Lazy.t;
        decl_pos: Pos_or_decl.t;
      }
    | Keyset_set of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Array_append of {
        pos: Pos.t;
        ty_name: string Lazy.t;
        decl_pos: Pos_or_decl.t;
      }
    | Const_mutation of {
        pos: Pos.t;
        ty_name: string Lazy.t;
        decl_pos: Pos_or_decl.t;
      }
    | Expected_class of {
        pos: Pos.t;
        suffix: string Lazy.t option;
      }
    | Unknown_type of {
        pos: Pos.t;
        expected: string;
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Parent_in_trait of Pos.t
    | Parent_undefined of {
        pos: Pos.t;
        trait_reqs: Pos_or_decl.t list option;
      }
    | Constructor_no_args of Pos.t
    | Visibility of {
        pos: Pos.t;
        msg: string;
        decl_pos: Pos_or_decl.t;
        reason_msg: string;
      }
    | Bad_call of {
        pos: Pos.t;
        ty_name: string Lazy.t;
      }
    | Extend_final of {
        pos: Pos.t;
        name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Extend_sealed of {
        pos: Pos.t;
        parent_pos: Pos_or_decl.t;
        parent_name: string;
        parent_kind: [ `intf | `trait | `class_ | `enum | `enum_class ];
        verb: [ `extend | `implement | `use ];
      }
    | Trait_prop_const_class of {
        pos: Pos.t;
        name: string;
      }
    | Implement_abstract of {
        pos: Pos.t;
        is_final: bool;
        decl_pos: Pos_or_decl.t;
        trace: Pos_or_decl.t Message.t list Lazy.t;
        name: string;
        kind: [ `meth | `prop | `const | `ty_const ];
        quickfixes: Pos.t Quickfix.t list;
      }
    | Abstract_member_in_concrete_class of {
        pos: Pos.t;
        class_name_pos: Pos.t;
        is_final: bool;
        member_kind: [ `method_ | `property | `constant | `type_constant ];
        member_name: string;
      }
    | Generic_static of {
        pos: Pos.t;
        typaram_name: string;
      }
    | Object_string of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Object_string_deprecated of Pos.t
    | Cyclic_typedef of {
        def_pos: Pos.t;
        use_pos: Pos_or_decl.t;
      }
    | Require_args_reify of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Require_generic_explicit of {
        pos: Pos.t;
        param_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Invalid_reified_arg of {
        pos: Pos.t;
        param_name: string;
        decl_pos: Pos_or_decl.t;
        arg_info: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Invalid_reified_arg_reifiable of {
        pos: Pos.t;
        param_name: string;
        decl_pos: Pos_or_decl.t;
        ty_pos: Pos_or_decl.t;
        ty_msg: string Lazy.t;
      }
    | New_class_reified of {
        pos: Pos.t;
        class_kind: string;
        suggested_class_name: string option;
      }
    | Class_get_reified of Pos.t
    | Static_meth_with_class_reified_generic of {
        pos: Pos.t;
        generic_pos: Pos.t;
      }
    | Consistent_construct_reified of Pos.t
    | Bad_fn_ptr_construction of Pos.t
    | Reified_generics_not_allowed of Pos.t
    | New_without_newable of {
        pos: Pos.t;
        name: string;
      }
    | Discarded_awaitable of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
      }
    | Unknown_object_member of {
        pos: Pos.t;
        member_name: string;
        elt: [ `meth | `prop ];
        reason: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Non_class_member of {
        pos: Pos.t;
        member_name: string;
        elt: [ `meth | `prop ];
        ty_name: string Lazy.t;
        decl_pos: Pos_or_decl.t;
      }
    | Null_container of {
        pos: Pos.t;
        null_witness: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Declared_covariant of {
        pos: Pos.t;
        param_pos: Pos.t;
        msgs: Pos.t Message.t list Lazy.t;
      }
    | Declared_contravariant of {
        pos: Pos.t;
        param_pos: Pos.t;
        msgs: Pos.t Message.t list Lazy.t;
      }
    | Static_prop_type_generic_param of {
        pos: Pos.t;
        var_ty_pos: Pos_or_decl.t;
        class_pos: Pos_or_decl.t;
      }
    | Contravariant_this of {
        pos: Pos.t;
        class_name: string;
        typaram_name: string;
      }
    | Cyclic_typeconst of {
        pos: Pos.t;
        tyconst_names: string list;
      }
    | Array_get_with_optional_field of {
        recv_pos: Pos.t;
        field_pos: Pos.t;
        field_name: string;
        decl_pos: Pos_or_decl.t;
      }
    | Mutating_const_property of Pos.t
    | Self_const_parent_not of Pos.t
    | Unexpected_ty_in_tast of {
        pos: Pos.t;
        expected_ty: string Lazy.t;
        actual_ty: string Lazy.t;
      }
    | Call_lvalue of Pos.t
    | Unsafe_cast_await of Pos.t
    (* == Primary and secondary =============================================== *)
    | Smember_not_found of {
        pos: Pos.t;
        kind:
          [ `class_constant
          | `class_typeconst
          | `class_variable
          | `static_method
          ];
        class_name: string;
        class_pos: Pos_or_decl.t;
        member_name: string;
        hint: ([ `instance | `static ] * Pos_or_decl.t * string) option;
        quickfixes: Pos.t Quickfix.t list;
      }
    | Type_arity_mismatch of {
        pos: Pos.t;
        actual: int;
        decl_pos: Pos_or_decl.t;
        expected: int;
      }
    | Typing_too_many_args of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
      }
    | Typing_too_few_args of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
        (* Hint for adding missing optional arguments, takes positions of
           declaration for optional arguments *)
        hint_missing_optional: Pos_or_decl.t option;
      }
    | Missing_named_args of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        missing_names: string list;
      }
    | Unexpected_named_args of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        unexpected_names: string list;
      }
    | Duplicate_named_args of {
        pos: Pos.t;
        duplicate_names: string list;
      }
    | Non_object_member of {
        pos: Pos.t;
        decl_pos: Pos_or_decl.t;
        ty_name: string Lazy.t;
        ctxt: [ `read | `write ];
        member_name: string;
        kind: [ `class_typeconst | `method_ | `property ];
      }
    | Static_instance_intersection of {
        class_pos: Pos.t;
        instance_pos: Pos_or_decl.t Lazy.t;
        static_pos: Pos_or_decl.t Lazy.t;
        member_name: string;
        kind: [ `meth | `prop ];
      }
    | Match_not_exhaustive of {
        pos: Pos.t;
        ty_not_covered: string Lazy.t;
      }
    | Match_on_unsupported_type of {
        pos: Pos.t;
        expr_ty: string Lazy.t;
        unsupported_tys: string Lazy.t list;
      }
    | Class_const_to_string of {
        pos: Pos.t;
        cls_name: string;
      }
    | Class_pointer_to_string of {
        pos: Pos.t;
        ty: string;
      }
    | String_to_class_pointer of {
        pos: Pos.t;
        cls_name: string;
        ty_pos: Pos_or_decl.t;
      }
    | Optional_parameter_not_supported of Pos.t
    | Optional_parameter_not_abstract of Pos.t
  [@@deriving show]
end

module rec Error : sig
  type t =
    | Primary of Primary.t
    | Apply of Callback.t * t
    | Apply_reasons of Reasons_callback.t * Secondary.t
    | Assert_in_current_decl of Secondary.t * Pos_or_decl.ctx
    | Multiple of t list
    | Union of t list
    | Intersection of t list
    | With_code of t * Error_code.t
    | With_pos of t * Pos.t
  [@@deriving show]
end

and Secondary : sig
  (** Specific error information which needs to be given a primary position from
       the AST being typed to be transformable into a user error.
       This can be done via applying a [Reasons_callback.t] using
       [apply_reasons].
  *)
  module Shape_splat_cycle : sig
    type edge_kind =
      | Direct_upper
      | Indirect_upper
      | Nested_upper
      | Nested_lower
    [@@deriving show]

    type edge = {
      pos: Pos_or_decl.t;
      source: Typing_defs_core.locl_ty;
      target: Typing_defs_core.locl_ty;
      kind: edge_kind;
    }
    [@@deriving show]
  end

  type t =
    | Of_error of Error.t
    (* == Primary and secondary =============================================== *)
    | Smember_not_found of {
        pos: Pos_or_decl.t;
        kind:
          [ `class_constant
          | `class_typeconst
          | `class_variable
          | `static_method
          ];
        class_name: string;
        class_pos: Pos_or_decl.t;
        member_name: string;
        hint: ([ `instance | `static ] * Pos_or_decl.t * string) option;
      }
    | Type_arity_mismatch of {
        pos: Pos_or_decl.t;
        actual: int;
        decl_pos: Pos_or_decl.t;
        expected: int;
      }
    | Typing_too_many_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
      }
    | Typing_too_few_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
      }
    | Non_object_member of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        ty_name: string Lazy.t;
        ctxt: [ `read | `write ];
        member_name: string;
        kind: [ `class_typeconst | `method_ | `property ];
      }
    | Rigid_tvar_escape of {
        pos: Pos_or_decl.t;
        name: string;
      }
    | Concrete_const_interface_override of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
        parent_origin: string;
        name: string;
      }
    | Interface_or_trait_const_multiple_defs of {
        pos: Pos_or_decl.t;
        origin: string;
        name: string;
        parent_pos: Pos_or_decl.t;
        parent_origin: string;
      }
    | Interface_typeconst_multiple_defs of {
        pos: Pos_or_decl.t;
        origin: string;
        is_abstract: bool;
        name: string;
        parent_pos: Pos_or_decl.t;
        parent_origin: string;
      }
    | Visibility_extends of {
        pos: Pos_or_decl.t;
        vis: string;
        parent_pos: Pos_or_decl.t;
        parent_vis: string;
      }
    | Visibility_override_internal of {
        pos: Pos_or_decl.t;
        module_name: string option;
        parent_pos: Pos_or_decl.t;
        parent_module: string;
      }
    | Abstract_tconst_not_allowed of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        tconst_name: string;
      }
    | Missing_constructor of Pos_or_decl.t
    | Missing_field of {
        pos: Pos_or_decl.t;
        name: string;
        decl_pos: Pos_or_decl.t;
        reason_sub: Typing_reason.t;
        reason_super: Typing_reason.t;
      }
    | Shape_fields_unknown of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Splat_may_require_fields of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        name: string option;
      }
    | Splat_field_not_known of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        name: string;
        field: string;
      }
    | Unsupported_cyclic_shape_splat_bounds of {
        pos: Pos_or_decl.t;
        cycle: Shape_splat_cycle.edge list;
      }
    | Accept_disposable_invariant of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Invalid_destructure of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        ty_name: string Lazy.t;
      }
    | Unpack_array_required_argument of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Unpack_array_variadic_argument of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Fun_too_many_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
      }
    | Fun_too_few_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        actual: int;
        expected: int;
      }
    | Fun_unexpected_nonvariadic of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Fun_variadicity_hh_vs_php56 of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Fun_extra_named_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        extra_names: string list;
      }
    | Fun_missing_named_args of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        missing_names: string list;
      }
    | Fun_param_required_but_expected_optional of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        (* If empty, this is a positional parameter *)
        param_names: string list;
      }
    | Required_field_is_optional of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
        def_pos: Pos_or_decl.t;
        name: string;
        reason_sub: Typing_reason.t;
        reason_super: Typing_reason.t;
      }
    | Return_disposable_mismatch of {
        pos_sub: Pos_or_decl.t;
        is_marked_return_disposable: bool;
        pos_super: Pos_or_decl.t;
      }
    | Overriding_prop_const_mismatch of {
        pos: Pos_or_decl.t;
        is_const: bool;
        parent_pos: Pos_or_decl.t;
        parent_is_const: bool;
      }
    | Override_final of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
    | Override_tests_bypass_visibility of {
        pos: Pos_or_decl.t;
        member_name: string;
        parent_pos: Pos_or_decl.t;
      }
    | Abstract_tests_bypass_visibility_missing_attr of {
        pos: Pos_or_decl.t;
        member_name: string;
        parent_pos: Pos_or_decl.t;
      }
    | Override_async of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
    | Override_lsb of {
        pos: Pos_or_decl.t;
        member_name: string;
        parent_pos: Pos_or_decl.t;
      }
    | Multiple_concrete_defs of {
        pos: Pos_or_decl.t;
        name: string;
        origin: string;
        class_name: string;
        parent_pos: Pos_or_decl.t;
        parent_origin: string;
      }
    | Cyclic_enum_constraint of Pos_or_decl.t
    | Inoutness_mismatch of {
        pos: Pos_or_decl.t;
        decl_pos: Pos_or_decl.t;
      }
    | Bad_lateinit_override of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
        parent_is_lateinit: bool;
      }
    | Bad_method_override of {
        pos: Pos_or_decl.t;
        member_name: string;
      }
    | Bad_member_override_not_subtype of {
        is_method: bool;
        class_name: string;
        parent_name: string;
        parent_type: string lazy_t;
        member_pos: Pos_or_decl.t;
        member_name: string;
        member_type: string lazy_t;
        origin_name: string;
        origin_type: string lazy_t;
        member_parent_pos: Pos_or_decl.t;
        member_parent_type: string lazy_t;
        member_parent_origin: string;
        member_parent_origin_type: string lazy_t;
      }
    | Bad_prop_override of {
        pos: Pos_or_decl.t;
        member_name: string;
      }
    | Bad_xhp_attr_required_override of {
        pos: Pos_or_decl.t;
        tag: string;
        parent_pos: Pos_or_decl.t;
        parent_tag: string;
      }
    | Coeffect_subtyping of {
        pos: Pos_or_decl.t;
        cap: string Lazy.t;
        pos_expected: Pos_or_decl.t;
        cap_expected: string Lazy.t;
      }
    | Override_method_support_dynamic_type of {
        pos: Pos_or_decl.t;
        method_name: string;
        parent_pos: Pos_or_decl.t;
        parent_origin: string;
      }
    | Readonly_mismatch of {
        pos: Pos_or_decl.t;
        kind: [ `fn | `fn_return | `param ];
        reason_sub: Pos_or_decl.t Message.t list Lazy.t;
        reason_super: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Require_package_mismatch of {
        pos: Pos_or_decl.t;
        reason_sub: Pos_or_decl.t Message.t list Lazy.t;
        reason_super: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Not_sub_dynamic of {
        pos: Pos_or_decl.t;
        ty_name: string Lazy.t;
        dynamic_part: Pos_or_decl.t Message.t list Lazy.t;
      }
    | Subtyping_error of {
        cstrs: (Pos_or_decl.t * Pos_or_decl.t Message.t) list Lazy.t;
        ty_sub: Typing_defs_constraints.internal_type;
        ty_sup: Typing_defs_constraints.internal_type;
        is_coeffect: bool;
      }
    | Method_not_dynamically_callable of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
    | This_final of {
        pos_sub: Pos_or_decl.t;
        pos_super: Pos_or_decl.t;
        class_name: string;
      }
    | Typeconst_concrete_concrete_override of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
    | Abstract_concrete_override of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
        kind: [ `constant | `method_ | `property | `typeconst ];
      }
    | Override_no_default_typeconst of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
    | Unsupported_refinement of Pos_or_decl.t
    | Splat_not_a_shape of Pos_or_decl.t
    | Missing_class_constant of {
        pos: Pos_or_decl.t;
        class_name: string;
        const_name: string;
      }
    | Invalid_refined_const_kind of {
        pos: Pos_or_decl.t;
        class_name: string;
        const_name: string;
        correct_kind: string;
        wrong_kind: string;
      }
    | Inexact_tconst_access of Pos_or_decl.t * (Pos_or_decl.t * string)
    | Violated_refinement_constraint of {
        cstr: [ `As | `Super ] * Pos_or_decl.t;
      }
    | Unknown_label of {
        enum_name: string;
        decl_pos: Pos_or_decl.t;
        most_similar: (string * Pos_or_decl.t) option;
      }
    | Higher_rank_tparam_escape of {
        tvar_pos: Pos_or_decl.t;
        pos_with_generic: Pos_or_decl.t;
        generic_reason: Typing_reason.t;
        generic_name: string option;
      }
    | Override_sealed of {
        pos: Pos_or_decl.t;
        parent_pos: Pos_or_decl.t;
      }
  [@@deriving show]
end

and Callback : sig
  (** A mechanism to apply transformations to primary errors *)
  type t =
    | Always of Primary.t
    | Of_primary of Primary.t
    | With_claim_as_reason of t * Primary.t
    | With_code of Error_code.t * Pos.t Quickfix.t list
    | Retain_code of t
    | With_side_effect of t * (unit -> unit)
  [@@deriving show]
  (* -- Constructors -------------------------------------------------------- *)

  (** Ignore any arguments and always return the given base error *)
  val always : Primary.t -> t

  (** Perform an aribrary side-effects when the error is evaluated *)
  val with_side_effect : t -> eff:(unit -> unit) -> t
    [@@ocaml.deprecated
      "This function will be removed. Please avoid adding side effects to error callbacks."]

  (** Provide default code, claim, reasons and quickfixes from the supplied
      `Primary.t` error *)
  val of_primary_error : Primary.t -> t

  (** Provide a default error code *)
  val with_code : code:Error_code.t -> t

  (** Ignore any incoming error code argument and use the existing one *)
  val retain_code : t -> t

  (** Fix the claim of the callback to `new_claim` and add the received claim
      argument to the list of reasons *)
  val with_claim_as_reason : t -> new_claim:Primary.t -> t

  (* -- Specific callbacks -------------------------------------------------- *)

  val unify_error : t

  val index_type_mismatch : t

  val covariant_index_type_mismatch : t

  val expected_stringlike : t

  val constant_does_not_match_enum_type : t

  val enum_underlying_type_must_be_arraykey : t

  val enum_constraint_must_be_arraykey : t

  val enum_subtype_must_have_compatible_constraint : t

  val parameter_default_value_wrong_type : t

  val newtype_alias_must_satisfy_constraint : t

  val missing_return : t

  val inout_return_type_mismatch : t

  val class_constant_value_does_not_match_hint : t

  val class_property_initializer_type_does_not_match_hint : t

  val xhp_attribute_does_not_match_hint : t

  val strict_str_concat_type_mismatch : t

  val strict_str_interp_type_mismatch : t

  val bitwise_math_invalid_argument : t

  val inc_dec_invalid_argument : t

  val math_invalid_argument : t

  val expect_bool_for_condition : t

  val using_error : Pos.t -> has_await:bool -> t
end

and Reasons_callback : sig
  (** A mechanism to apply transformations to secondary errors *)
  type op =
    | Append
    | Prepend
  [@@deriving show]

  type component =
    | Code
    | Reasons
    | Quickfixes
  [@@deriving show, eq]

  type t =
    | Always of Error.t
    | Of_error of Error.t
    | Prefix of Primary.t
    | Of_callback of Callback.t * Pos.t Message.t Lazy.t
    | Retain of t * component
    | Incoming_reasons of t * op
    | With_code of t * Error_code.t
    | With_claim of t * Pos.t Message.t Lazy.t
    | With_reasons of t * Pos_or_decl.t Message.t list Lazy.t
    | Add_quickfixes of t * Pos.t Quickfix.t list
    | Add_reason of t * op * Pos_or_decl.t Message.t Lazy.t
    | From_on_error of
        ((?code:int ->
         ?quickfixes:Pos.t Quickfix.t list ->
         Pos_or_decl.t Message.t list ->
         unit)
        [@show.opaque])
        [@ocaml.deprecated
          "This constructor will be removed. Please use the provided combinators for constructing error callbacks."]
    | Prepend_on_apply of t * Secondary.t
    | Assert_in_current_decl of Error_code.t * Pos_or_decl.ctx
    | Drop_reasons_on_apply of t
    | Choose of bool Lazy.t * t * t
  [@@deriving show]

  (* -- Constructors -------------------------------------------------------- *)

  (** Construct a `Reasons_callback.t` from a side-effecting function. This is
      included to support legacy code only and will be removed once that code is
      migrated *)
  val from_on_error :
    (?code:int ->
    ?quickfixes:Pos.t Quickfix.t list ->
    Pos_or_decl.t Message.t list ->
    unit) ->
    t
    [@@ocaml.deprecated
      "This function will be removed. Please use the provided combinators for constructing error callbacks."]

  (** Create a constant `Reasons_callback.t` which defaults to the value of the
      supplied `Error.t` when applied. Each component of that error may be
      subsequently overridden if a value for that component is supplied during
      evaluation *)
  val of_error : Error.t -> t

  (** Create a `Reasons_callback.t` which defaults to the value of the supplied
      `Primary.t` error when applied; this is equivalent
       to `of_error @@ primary err`
  *)
  val of_primary_error : Primary.t -> t

  (** Select a callback when the error is evaluated. *)
  val choose : bool Lazy.t -> if_true:t -> if_false:t -> t

  (** Replace the current default claim with the claim from the supplied
      `Pos.t Message.t` *)
  val with_claim : Callback.t -> claim:Pos.t Message.t Lazy.t -> t

  (** Replace the current default error code with the supplied one *)
  val with_code : t -> code:Error_code.t -> t

  (** Replace the current list of reasons with those supplied. This is typically
       used in combination with `retain_reasons` to fix the `reasons` of the
      `User_diagnostic.t` that is obtained when the callback is applied
  *)
  val with_reasons : t -> reasons:Pos_or_decl.t Message.t list Lazy.t -> t

  (** Add a `quickfix` to the `User_diagnostic.t` generated when the callback is
      applied
  *)
  val add_quickfixes : t -> Pos.t Quickfix.t list -> t

  (** Add the `reason` to the start of current list of reasons *)
  val prepend_reason : t -> reason:Pos_or_decl.t Message.t Lazy.t -> t

  (** Add the `reason` to the end of current list of reasons *)
  val append_reason : t -> reason:Pos_or_decl.t Message.t Lazy.t -> t

  (** When applied, append the supplied reasons to the current list of reasons *)
  val append_incoming_reasons : t -> t

  (** When applied, prepend the supplied reasons to the current list of reasons *)
  val prepend_incoming_reasons : t -> t

  (** Ignore the incoming error code argument when evaluating to a `User_diagnostic.t`.

      This is equivalent to the following function, given some callback `on_error`:
      `(fun ?code:_ ?quickfixes reasons -> on_error ?quickfixes reasons)`
  *)
  val retain_code : t -> t

  (** Ignore the incoming reasons argument when evaluating to a `User_diagnostic.t`.

      This is equivalent to the following function, given some callback `on_error`:
      `(fun ?code ?quickfixes _ -> on_error ?code ?quickfixes)`
  *)
  val retain_reasons : t -> t

  (** Ignore the incoming quickfixes component when evaluating to a `User_diagnostic.t`.

      This is equivalent to the following function, given some callback `on_error`:
      `(fun ?code ?quickfixes:_ reasons -> on_error ?code reasons)`
  *)
  val retain_quickfixes : t -> t

  (** Create a constant `Reasons_callback.t` which always evaluates to the
      supplied `Error.t` when applied

      This is equivalent to the following function, for some `error` of type
      `Error.t`:
      `(fun ?code:_ ?quickfixes:_ _ -> error)`

      This is also the same as:
      `retain_code @@ retain_reasons @@ reatain_quickfixes @@ of_error error`
  *)
  val always : Error.t -> t

  (** Applying the [Reasons_callback.t] [(prepend_on_apply err snd_err1) snd_err2]
       is the same as applying `err` the secondary error created by prepending
       `snd_err1` to `snd_err2`.

       The `Secondary.t` error created by the prepending operation has the same
       code of `snd_err1` and the reasons from `snd_err1` prepended to those of
       `snd_err2`.
  *)
  val prepend_on_apply : t -> Secondary.t -> t

  (** Creates a callback that ignores the secondary reasons that are passed
      when it is applied and then proceeds with the callback it wraps. This
      construct is only helpful if the wrapped callback contains enough
      context to generate a usable error message. *)
  val drop_reasons_on_apply : t -> t

  (* Applying the `Reasons_callback.t` `(assert_in_current_decl code ctx) err`
     will evaluate the `Secondary.t` `err` then use the head of the list of
     reasons as claim in the resulting error, given the position for that
     reason is in the current decl. *)
  val assert_in_current_decl : Error_code.t -> ctx:Pos_or_decl.ctx -> t

  (* -- Specific callbacks -------------------------------------------------- *)

  val unify_error_at : Pos.t -> t

  val expr_tree_splice_error :
    Pos.t ->
    expr_pos:Pos_or_decl.t ->
    contextual_reasons:Pos_or_decl.t Message.t list Lazy.t option ->
    dsl_opt:string option ->
    docs_url:string option Lazy.t ->
    t

  val bad_enum_decl : Pos.t -> t

  val bad_conditional_support_dynamic :
    Pos.t ->
    child:string ->
    parent:string ->
    ty_name:string Lazy.t ->
    self_ty_name:string Lazy.t ->
    t

  val bad_decl_override :
    name:string -> parent_pos:Pos.t -> parent_name:string -> t

  val invalid_class_refinement : Pos.t -> t

  val explain_where_constraint :
    Pos.t -> in_class:bool -> decl_pos:Pos_or_decl.t -> t

  val explain_constraint : Pos.t -> t

  val rigid_tvar_escape_at : Pos.t -> string -> t

  val invalid_type_hint : Pos.t -> t

  val type_constant_mismatch : t -> t

  val class_constant_type_mismatch : t -> t

  val unsatisfied_req_callback :
    class_pos:Pos.t ->
    trait_pos:Pos_or_decl.t ->
    req_pos:Pos_or_decl.t ->
    string ->
    t

  val invalid_echo_argument_at : Pos.t -> t

  val index_type_mismatch_at : Pos.t -> t

  val unify_error_assert_primary_pos_in_current_decl : Pos_or_decl.ctx -> t

  val invalid_type_hint_assert_primary_pos_in_current_decl :
    Pos_or_decl.ctx -> t
end

type t = Error.t

val pp : Format.formatter -> t -> unit

val show : t -> string

(* -- Constructors -------------------------------------------------------- *)

(** Lift a `Primary.t` error to a `Typing_error.t` *)
val primary : Primary.t -> t

(** Lift a `Primary.Coeffect.t` error to a `Typing_error.t` *)
val coeffect : Primary.Coeffect.t -> t

(** Lift a `Primary.Enum.t` error to a `Typing_error.t` *)
val enum : Primary.Enum.t -> t

(** Lift a `Primary.Expr_tree.t` error to a `Typing_error.t` *)
val expr_tree : Primary.Expr_tree.t -> t

(** Lift a `Primary.Modules.t` error to a `Typing_error.t` *)
val modules : Primary.Modules.t -> t

(** Lift a `Primary.Package.t` error to a `Typing_error.t` *)
val package : Primary.Package.t -> t

(** Lift a `Primary.Readonly.t` error to a `Typing_error.t` *)
val readonly : Primary.Readonly.t -> t

(** Lift a `Primary.Shape.t` error to a `Typing_error.t` *)
val shape : Primary.Shape.t -> t

(** Lift a `Primary.Switch.t` error to a `Typing_error.t` *)
val switch : Primary.Switch.t -> t

(** Lift a `Primary.Wellformedness.t` error to a `Typing_error.t` *)
val wellformedness : Primary.Wellformedness.t -> t

(** Lift a `Primary.Xhp.t` error to a `Typing_error.t` *)
val xhp : Primary.Xhp.t -> t

(** Lift a `Primary.CaseType.t` error to a `Typing_error.t` *)
val casetype : Primary.CaseType.t -> t

(** Lift a `Primary.SimpliHack.t` error to a `Typing_error.t` *)
val simplihack : Primary.SimpliHack.t -> t

(** Apply a the `Reasons_callback.t` to the supplied `Secondary.t` error, using
    the reasons and error code associated with that error *)
val apply_reasons : Secondary.t -> on_error:Reasons_callback.t -> t

(** Apply a the `Callback.t` to the supplied `Secondary.t` error, using
    the claim, reasons, quickfixes and error code associated with that error *)
val apply : t -> on_error:Callback.t -> t

(** Lift a `Secondary.t` error to a `Typing_error.t` by asserting that the
     position of the first reason is in the current decl and treating it as a
     claim  *)
val assert_in_current_decl : Secondary.t -> ctx:Pos_or_decl.ctx -> t

(** Report a list of errors at each type of an intersection *)
val intersect : t list -> t

(** Calling `intersect_opt xs` returns `None` if the list is empty and `intersect xs` otherwise *)
val intersect_opt : t list -> t option

(** Report a list of errors at each type of a union*)
val union : t list -> t

(** Calling `union_opt xs` returns `None` if the list is empty and `union xs` otherwise *)
val union_opt : t list -> t option

(** Report multiple errors at a single type *)
val multiple : t list -> t

(** Calling `multiple_opt xs` returns `None` if the list is empty and `multiple xs` otherwise *)
val multiple_opt : t list -> t option

(** Report two errors at a single type; `both t1 t2` is the same as
    `multiple [t1;t2]`*)
val both : t -> t -> t

(** Modify the code that will be reported when evaluated to a `User_diagnostic.t`  *)
val with_code : t -> code:Error_code.t -> t

val count : t -> int
