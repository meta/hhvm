(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Aast
open Typing_defs

module Runtime_value_map = Stdlib.Map.Make (struct
  type t = Const_value.concrete_value

  let compare = Const_value.compare_concrete_value
end)

let symbolic_name_pos symbolic_name =
  match symbolic_name with
  | Ast_defs.SFlit_str (pos, _)
  | Ast_defs.SFclassname (pos, _)
  | Ast_defs.SFclass_const (_, (pos, _)) ->
    pos

let symbolic_name_span = function
  | Ast_defs.SFlit_str (pos, _)
  | Ast_defs.SFclassname (pos, _) ->
    pos
  | Ast_defs.SFclass_const ((class_pos, _), (member_pos, _)) ->
    Pos.btw class_pos member_pos

(* Keep in sync with hackc's [hhbc::ClassName::mangle] in [hackc/hhbc/id.rs],
   including [hhbc_string_utils::mangle] and [hhbc_string_utils::mangle_xhp_id]
   in [hackc/utils/string_utils.rs]. *)
let runtime_class_name class_name =
  let class_name = Utils.strip_ns class_name in
  let (namespace, name) =
    match String.rsplit2 class_name ~on:'\\' with
    | Some (namespace, name) -> (namespace ^ "\\", name)
    | None -> ("", class_name)
  in
  let name =
    if String.is_prefix name ~prefix:":" then
      "xhp_"
      ^ (String.drop_prefix name 1
        |> String.substr_replace_all ~pattern:":" ~with_:"__"
        |> String.substr_replace_all ~pattern:"-" ~with_:"_")
    else
      name
  in
  namespace ^ name

let resolve_runtime_value env = function
  | Ast_defs.SFlit_str (_, value) -> Some (Const_value.SKVString value)
  | Ast_defs.SFclassname (_, class_name) ->
    let (_ : Folded_class.t Decl_entry.t) = Tast_env.get_class env class_name in
    Some (Const_value.SKVString (runtime_class_name class_name))
  | Ast_defs.SFclass_const ((_, class_name), (_, member_name)) ->
    (match Tast_env.get_class env class_name with
    | Decl_entry.Found cls -> Tast_env.get_const env cls member_name
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      None)
    |> Option.bind ~f:(fun const ->
           match const.cc_abstract with
           | CCAbstract _ -> None
           | CCConcrete ->
             Const_value.concrete_value ~member_name const.cc_value)

let report_duplicate_symbolic_name
    custom_err_config ~symbolic_name ~first_symbolic_name =
  let diagnostic =
    Naming_error_utils.to_user_diagnostic
      (Naming_error.Field_name_already_bound (symbolic_name_pos symbolic_name))
      custom_err_config
  in
  Diagnostics.add_diagnostic
    {
      diagnostic with
      User_diagnostic.reasons =
        [
          ( Pos_or_decl.of_raw_pos (symbolic_name_pos first_symbolic_name),
            "The conflicting shape field is here" );
        ];
    }

let report_duplicate_runtime_value
    custom_err_config ~symbolic_name ~first_symbolic_name runtime_value =
  if Ast_defs.ShapeField.equal symbolic_name first_symbolic_name then
    report_duplicate_symbolic_name
      custom_err_config
      ~symbolic_name
      ~first_symbolic_name
  else
    let runtime_value_text =
      match runtime_value with
      | Const_value.SKVInt value
      | Const_value.SKVString value ->
        value
    in
    let diagnostic =
      Naming_error_utils.to_user_diagnostic
        (Naming_error.Field_name_already_bound
           (symbolic_name_span symbolic_name))
        custom_err_config
    in
    Diagnostics.add_diagnostic
      {
        diagnostic with
        User_diagnostic.reasons =
          [
            ( Pos_or_decl.of_raw_pos (symbolic_name_span first_symbolic_name),
              "This field has the same runtime key "
              ^ Markdown_lite.md_codify runtime_value_text );
          ];
      }

let check_duplicates env symbolic_names =
  let tcopt = Tast_env.get_tcopt env in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let check_field (seen_symbolic_names, seen_runtime_values) symbolic_name =
    let runtime_value = resolve_runtime_value env symbolic_name in
    let seen_runtime_values =
      match runtime_value with
      | None ->
        (match Ast_defs.ShapeSet.find_opt symbolic_name seen_symbolic_names with
        | None -> ()
        | Some first_symbolic_name ->
          report_duplicate_symbolic_name
            custom_err_config
            ~symbolic_name
            ~first_symbolic_name);
        seen_runtime_values
      | Some runtime_value -> begin
        match Runtime_value_map.find_opt runtime_value seen_runtime_values with
        | None ->
          Runtime_value_map.add runtime_value symbolic_name seen_runtime_values
        | Some first_symbolic_name ->
          report_duplicate_runtime_value
            custom_err_config
            ~symbolic_name
            ~first_symbolic_name
            runtime_value;
          seen_runtime_values
      end
    in
    let seen_symbolic_names =
      if Ast_defs.ShapeSet.mem symbolic_name seen_symbolic_names then
        seen_symbolic_names
      else
        Ast_defs.ShapeSet.add symbolic_name seen_symbolic_names
    in
    (seen_symbolic_names, seen_runtime_values)
  in
  let (_ : Ast_defs.ShapeSet.t * Ast_defs.shape_field_name Runtime_value_map.t)
      =
    List.fold_left
      symbolic_names
      ~init:(Ast_defs.ShapeSet.empty, Runtime_value_map.empty)
      ~f:check_field
  in
  ()

let destructure_visitor =
  object
    inherit Tast_visitor.iter as super

    method! on_destructure_shape env shape_pat =
      check_duplicates
        env
        (List.map shape_pat.ds_fields ~f:(fun field -> field.dsf_name));
      super#on_destructure_shape env shape_pat
  end

let handler =
  object
    inherit Tast_visitor.handler_base

    method! at_expr env expr =
      match expr with
      | (_, _, Shape fields) ->
        let symbolic_names =
          List.filter_map fields ~f:(function
              | SF_field (symbolic_name, _) -> Some symbolic_name
              | SF_splat _ -> None)
        in
        check_duplicates env symbolic_names
      | (_, _, DestructureShape shape_pat) ->
        destructure_visitor#on_destructure_shape env shape_pat
      | (_, _, DestructureTuple tuple_pat) ->
        destructure_visitor#on_destructure_tuple env tuple_pat
      | _ -> ()

    method! at_hint env (_, hint) =
      match hint with
      | Hshape { nsi_field_map; _ } ->
        let symbolic_names =
          List.filter_map nsi_field_map ~f:(function
              | SE_field { sfi_name; _ } -> Some sfi_name
              | SE_splat _ -> None)
        in
        check_duplicates env symbolic_names
      | _ -> ()
  end
