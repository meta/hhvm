(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

module Elab_happly_hint = struct
  type t = { tparams: Pos.t S_map.t }

  let empty = { tparams = S_map.empty }
end

module Elab_func_body = struct
  type t = { in_mode: File_info.mode }

  let empty = { in_mode = File_info.Mstrict }
end

module Elab_haccess_hint = struct
  type t = {
    current_class: (Ast_defs.id * Ast_defs.classish_kind * bool) option;
    in_where_clause: bool;
    in_generated_where_clause: bool;
    in_context: bool;
    in_haccess: bool;
  }

  let empty =
    {
      current_class = None;
      in_where_clause = false;
      in_generated_where_clause = false;
      in_context = false;
      in_haccess = false;
    }
end

module Elab_this_hint = struct
  type t = {
    (* `this` is forbidden as a hint in this context *)
    forbid_this: bool;
    lsb: bool option;
    in_interface: bool;
    in_req_extends: bool;
    in_extends: bool;
    is_top_level_haccess_root: bool;
    in_invariant_final: bool;
  }

  let empty =
    {
      forbid_this = false;
      lsb = None;
      in_interface = false;
      in_req_extends = false;
      in_extends = false;
      is_top_level_haccess_root = false;
      in_invariant_final = false;
    }
end

module Elab_call = struct
  type t = {
    current_class: (Ast_defs.id * Ast_defs.classish_kind * bool) option;
  }

  let empty = { current_class = None }
end

module Elab_class_id = struct
  type t = { in_class: bool }

  let empty = { in_class = false }
end

module Elab_const_expr = struct
  type t = {
    enforce_const_expr: bool;
    in_enum_class: bool;
    in_mode: File_info.mode;
  }

  let empty =
    {
      enforce_const_expr = false;
      in_enum_class = false;
      in_mode = File_info.Mstrict;
    }
end

module Elab_everything_sdt = struct
  type t = {
    in_is_as: bool;
    in_enum_class: bool;
    under_no_auto_dynamic: bool;
    under_no_auto_likes: bool;
  }

  let empty =
    {
      in_is_as = false;
      in_enum_class = false;
      under_no_auto_dynamic = false;
      under_no_auto_likes = false;
    }
end

module Elab_wildcard_hint = struct
  type t = {
    allow_wildcard: bool;
    tp_depth: int;
  }

  let empty = { allow_wildcard = false; tp_depth = 0 }
end

module Elab_shape_field_name = struct
  type t = {
    current_class: (Ast_defs.id * Ast_defs.classish_kind * bool) option;
  }

  let empty = { current_class = None }
end

module Elab_retonly_hint = struct
  type t = { allow_retonly: bool }

  let empty = { allow_retonly = false }
end

module Validate_toplevel_statement = struct
  type t = { in_class_or_fun_def: bool }

  let empty = { in_class_or_fun_def = false }
end

module Validate_polymorphic_lambda = struct
  type t = { in_poly_lambda: bool }

  let empty = { in_poly_lambda = false }
end

type t = {
  elab_happly_hint: Elab_happly_hint.t;
  elab_haccess_hint: Elab_haccess_hint.t;
  elab_class_id: Elab_class_id.t;
  elab_this_hint: Elab_this_hint.t;
  elab_call: Elab_call.t;
  elab_const_expr: Elab_const_expr.t;
  elab_everything_sdt: Elab_everything_sdt.t;
  elab_func_body: Elab_func_body.t;
  elab_retonly_hint: Elab_retonly_hint.t;
  elab_wildcard_hint: Elab_wildcard_hint.t;
  elab_shape_field_name: Elab_shape_field_name.t;
  validate_toplevel_statement: Validate_toplevel_statement.t;
  validate_polymorphic_lambda: Validate_polymorphic_lambda.t;
  everything_sdt: bool;
  soft_as_like: bool;
  consistent_ctor_level: int;
  supportdynamic_type_hint_enabled: bool;
  is_systemlib: bool;
  is_hhi: bool;
  allow_module_def: bool;
  allow_ignore_readonly: bool;
  named_variadic_type: bool;
  variadic_named_parameters: bool;
}

let empty =
  {
    elab_happly_hint = Elab_happly_hint.empty;
    elab_haccess_hint = Elab_haccess_hint.empty;
    elab_class_id = Elab_class_id.empty;
    elab_this_hint = Elab_this_hint.empty;
    elab_call = Elab_call.empty;
    elab_const_expr = Elab_const_expr.empty;
    elab_everything_sdt = Elab_everything_sdt.empty;
    elab_func_body = Elab_func_body.empty;
    elab_retonly_hint = Elab_retonly_hint.empty;
    elab_wildcard_hint = Elab_wildcard_hint.empty;
    elab_shape_field_name = Elab_shape_field_name.empty;
    validate_toplevel_statement = Validate_toplevel_statement.empty;
    validate_polymorphic_lambda = Validate_polymorphic_lambda.empty;
    everything_sdt = false;
    soft_as_like = false;
    consistent_ctor_level = 0;
    supportdynamic_type_hint_enabled = false;
    is_systemlib = false;
    is_hhi = false;
    allow_module_def = false;
    allow_ignore_readonly = false;
    named_variadic_type = false;
    variadic_named_parameters = false;
  }
