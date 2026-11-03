(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

module Error_code = Error_codes.NastCheck

type verb =
  | Vreq_implement
  | Vimplement

(** The construct that asked whether a package is deployed. Rendered (with
    styling) by the error constructor. *)
type package_construct =
  | Package_expression
  | Require_package_attribute of string

type t =
  | Repeated_record_field_name of {
      pos: Pos.t;
      name: string;
      prev_pos: Pos_or_decl.t;
    }
  | Dynamically_callable_reified of Pos.t
  | No_construct_parent of Pos.t
  | Nonstatic_method_in_abstract_final_class of Pos.t
  | Constructor_required of {
      pos: Pos.t;
      class_name: string;
      prop_names: string list;
    }
  | Not_initialized of {
      pos: Pos.t;
      class_name: string;
      props: (Pos_or_decl.t * string) list;
    }
  | Call_before_init of {
      pos: Pos.t;
      prop_name: string;
    }
  | Abstract_with_body of Pos.t
  | Not_abstract_without_typeconst of Pos.t
  | Typeconst_depends_on_external_tparam of {
      pos: Pos.t;
      ext_pos: Pos.t;
      ext_name: string;
    }
  | Interface_with_partial_typeconst of Pos.t
  | Partially_abstract_typeconst_definition of Pos.t
  | Refinement_in_typestruct of {
      pos: Pos.t;
      kind: string;
    }
  | Multiple_xhp_category of Pos.t
  | Return_in_finally of Pos.t
  | Toplevel_break of Pos.t
  | Toplevel_continue of Pos.t
  | Continue_in_switch of Pos.t
  | Await_in_sync_function of {
      pos: Pos.t;
      func_pos: Pos.t option;
      keyword: string;
    }
  | Interface_uses_trait of Pos.t
  | Magic of {
      pos: Pos.t;
      meth_name: string;
    }
  | Non_interface of {
      pos: Pos.t;
      name: string;
      verb: verb;
    }
  | ToString_returns_string of Pos.t
  | ToString_visibility of Pos.t
  | Uses_non_trait of {
      pos: Pos.t;
      name: string;
      kind: string;
    }
  | Requires_non_class of {
      pos: Pos.t;
      name: string;
      kind: string;
      is_interface: bool;
      is_req_this_as_check: bool;
    }
  | Requires_final_class of {
      pos: Pos.t;
      name: string;
      is_req_this_as_check: bool;
    }
  | Abstract_body of Pos.t
  | Interface_with_member_variable of Pos.t
  | Interface_with_static_member_variable of Pos.t
  | Illegal_function_name of {
      pos: Pos.t;
      name: string;
    }
  | Entrypoint_arguments of Pos.t
  | Entrypoint_generics of Pos.t
  | Variadic_memoize of Pos.t
  | Abstract_method_memoize of Pos.t
  | Instance_property_in_abstract_final_class of Pos.t
  | Inout_params_special of Pos.t
  | Inout_params_memoize of {
      pos: Pos.t;
      param_pos: Pos.t;
    }
  | Inout_in_transformed_pseudofunction of {
      pos: Pos.t;
      fn_name: string;
    }
  | Named_in_transformed_pseudofunction of {
      pos: Pos.t;
      fn_name: string;
    }
  | Reading_from_append of Pos.t
  | List_rvalue of Pos.t
  | Placeholder_read of Pos.t
  | Placeholder_inout_write of {
      pos: Pos.t;
      param_pos: Pos.t;
    }
  | Illegal_destructor of Pos.t
  | Illegal_context of {
      pos: Pos.t;
      name: string;
    }
  | Case_fallthrough of {
      switch_pos: Pos.t;
      case_pos: Pos.t;
      next_pos: Pos.t option;
    }
  | Default_fallthrough of Pos.t
  | Php_lambda_disallowed of Pos.t
  | Internal_method_with_invalid_visibility of {
      pos: Pos.t;
      vis: Ast_defs.visibility;
    }
  | Private_and_final of Pos.t
  | Internal_member_inside_public_trait of {
      member_pos: Pos.t;
      trait_pos: Pos.t;
      is_method: bool;
    }
  | Attribute_conflicting_memoize of {
      pos: Pos.t;
      second_pos: Pos.t;
    }
  | Soft_internal_without_internal of Pos.t
  | Wrong_expression_kind_builtin_attribute of {
      pos: Pos.t;
      attr_name: string;
      expr_kind: string;
    }
  | Attribute_too_many_arguments of {
      pos: Pos.t;
      name: string;
      expected: int;
    }
  | Attribute_too_few_arguments of {
      pos: Pos.t;
      name: string;
      expected: int;
    }
  | Attribute_not_exact_number_of_args of {
      pos: Pos.t;
      name: string;
      actual: int;
      expected: int;
    }
  | Attribute_param_type of {
      pos: Pos.t;
      x: string;
    }
  | Attribute_no_auto_dynamic of Pos.t
  | Attribute_implemented_by_restriction of {
      restriction: string;
      pos: Pos.t;
    }
  | Generic_at_runtime of {
      pos: Pos.t;
      prefix: string;
    }
  | Generics_not_allowed of Pos.t
  | Local_variable_modified_and_used of {
      pos: Pos.t;
      pos_useds: Pos.t list;
    }
  | Local_variable_modified_twice of {
      pos: Pos.t;
      pos_modifieds: Pos.t list;
    }
  | Assign_during_case of Pos.t
  | Read_before_write of {
      pos: Pos.t;
      member_name: string;
    }
  | Lateinit_with_default of Pos.t
  | Missing_assign of Pos.t
  | Clone_return_type of Pos.t
  | Class_sealed_with_trait of {
      pos: Pos.t;
      class_name: string;
      trait_name: string;
    }
  | Require_package_strict_inclusion of {
      required_pos: Pos.t;
      required: string;
      def_pos: Pos_or_decl.t;
      current: string;
      current_pos: Pos.t;
      attribute_name: string;
      current_package_assignment_kind: string;
    }
  | Package_expression_strict_inclusion of {
      pkg_pos: Pos.t;
      pkg: string;
      def_pos: Pos_or_decl.t;
      current: string;
      current_pos: Pos.t;
      current_package_assignment_kind: string;
    }
  | Observation_not_allowed_for_package of {
      pos: Pos.t;
      pkg: string;
      def_pos: Pos_or_decl.t;
      construct: package_construct;
    }
  | Override_not_allowed_for_package of {
      pos: Pos.t;
      pkg: string;
      def_pos: Pos_or_decl.t;
      attr: string;
    }
  | Package_override_target_not_included of {
      override_pos: Pos.t;
      target: string;
      path_package: string;
      path_package_pos: Pos_or_decl.t;
    }
  | Redundant_package_override of {
      pos: Pos.t;
      pkg: string;
      def_pos: Pos_or_decl.t;
    }

val to_user_diagnostic : t -> (Pos.t, Pos_or_decl.t) User_diagnostic.t
