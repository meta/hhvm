(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

[@@@warning "-33"]

open Hh_prelude

[@@@warning "+33"]

let logger_handlers =
  [
    ("tany", Tany_logger.create_handler);
    ("nothing_property", Nothing_property_logger.create_handler);
    ("fbid_igid_type", Fbid_igid_type_logger.create_handler);
    ("type_driven_code_health", Type_driven_code_health_logger.create_handler);
    ("list_logger", Analyze_list_logger.create_handler);
  ]

(* Handlers that are enabled through 'log_levels' configuration. *)
let select_logger_handlers ctx =
  let tco = Provider_context.get_tcopt ctx in
  let log_levels = Typechecker_options.log_levels tco in
  let add_handler handlers (key, handler) =
    match S_map.find_opt key log_levels with
    | Some level when level > 0 -> handler ctx :: handlers
    | _ -> handlers
  in
  List.fold ~init:[] ~f:add_handler logger_handlers

let warning_checks =
  [
    (module Is_check : Handler.Warning.S);
    (module Disjoint_types);
    (module Cast_non_primitive);
    (module Truthiness_test);
    (module Equality_check);
    (module Duplicate_properties);
    (module Redundant_nullsafe);
    (module Redundant_concurrent);
    (module Set_or_keyset_array_get);
    (module Tany_check);
    (module Dynamic_call_check);
    (module Dynamic_return_check);
  ]

let visitor ctx =
  (* Handlers that are not TAST checks to produce errors, but are used for
     telemetry that processes TASTs. *)
  let irregular_handlers = select_logger_handlers ctx in
  let tcopt = Provider_context.get_tcopt ctx in
  let hierarchy_check handler =
    if Typechecker_options.skip_hierarchy_checks tcopt then
      None
    else
      Some handler
  in

  let warning_handlers =
    List.filter_map warning_checks ~f:(fun (module M) ->
        if
          List.exists
            M.error_codes
            ~f:(Typing_warning_utils.code_is_enabled tcopt)
        then
          Some (M.handler ~as_lint:false)
        else
          None)
  in
  let handlers =
    irregular_handlers
    @ warning_handlers
    @ List.filter_map
        ~f:Fn.id
        [
          Xhp_required_check.create_handler ctx;
          Redundant_generics_check.create_handler ctx;
          Some Shape_field_check.handler;
          Some String_cast_check.handler;
          Some Tautology_check.handler;
          Some Enforceable_hint_check.handler;
          Some Const_write_check.handler;
          Some Switch_exhaustivity_check.handler;
          Some Void_return_check.handler;
          Some Rvalue_check.handler;
          Some Callconv_check.handler;
          Some Xhp_check.handler;
          Some Discarded_awaitable_check.handler;
          Some Invalid_index_check.handler;
          Some Pseudofunctions_check.handler;
          Some Reified_check.handler;
          Some Instantiability_check.handler;
          Some Static_memoized_check.handler;
          Some Abstract_class_check.handler;
          hierarchy_check Class_parent_check.handler;
          Some Method_type_param_check.handler;
          Some Obj_get_check.handler;
          Some This_hint_check.handler;
          Some Unresolved_type_variable_check.handler;
          Some Type_const_check.handler;
          Some Static_method_generics_check.handler;
          hierarchy_check Class_inherited_member_case_check.handler;
          Some Readonly_check.handler;
          Some Destructure_check.handler;
          Some Meth_caller_check.handler;
          Some Expression_tree_check.handler;
          Some Xhp_attr_value.handler;
          hierarchy_check Class_const_origin_check.handler;
          (if Typechecker_options.global_access_check_enabled tcopt then
            Some Global_access_check.handler
          else
            None);
          hierarchy_check Enum_check.handler;
          (if Typechecker_options.check_duplicate_enum_values tcopt then
            hierarchy_check Enum_duplicate_value_check.handler
          else
            None);
          (if Typechecker_options.populate_dead_unsafe_cast_heap tcopt then
            Some Remove_dead_unsafe_casts.patch_location_collection_handler
          else
            None);
          Some Class_pointer_check.handler;
          (if Typechecker_options.private_member_variance_check tcopt then
            Some Private_member_variance_check.handler
          else
            None);
          Some No_disjoint_union_check.handler;
          Some Simplihack_check.handler;
        ]
  in
  let handlers =
    if Typechecker_options.skip_tast_checks (Provider_context.get_tcopt ctx)
    then
      []
    else
      handlers
  in
  Tast_visitor.iter_with handlers

let program ctx = (visitor ctx)#go ctx

let def ctx = (visitor ctx)#go_def ctx

(** A visitor that only runs warning checks (no error-producing checks).
    Used to run warning checks on the dynamic TAST for trusted/untrusted
    intersection. *)
let warning_visitor ctx =
  let tcopt = Provider_context.get_tcopt ctx in
  let handlers =
    if Typechecker_options.skip_tast_checks tcopt then
      []
    else
      List.filter_map warning_checks ~f:(fun (module M) ->
          if
            List.exists
              M.error_codes
              ~f:(Typing_warning_utils.code_is_enabled tcopt)
          then
            Some (M.handler ~as_lint:false)
          else
            None)
  in
  Tast_visitor.iter_with ~catch_handler_exceptions:true handlers

let warning_def ctx = (warning_visitor ctx)#go_def ctx

let warning_program ctx = (warning_visitor ctx)#go ctx
