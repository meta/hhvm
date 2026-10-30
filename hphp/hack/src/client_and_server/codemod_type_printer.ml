(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
open Typing_defs

exception Non_denotable

let strip_ns str =
  let str' = Utils.strip_ns str in
  (* If we had more than one '\\' patternm then we must keep the first '\\'
     otherwise it will cause an error (we are detecting types from the root
     namespace)*)
  if String.contains str' '\\' then
    str
  else
    str'

let rec print_ty_exn ?(allow_nothing = false) ty =
  match get_node ty with
  | Tprim p -> Aast_defs.string_of_tprim p
  | Tunion [] when allow_nothing -> "nothing"
  | Tany _
  | Tvar _
  | Tdependent _
  | Tunion _
  | Tintersection _
  | Tlabel _
  | Tneg _ ->
    raise Non_denotable
  | Tnonnull -> "nonnull"
  | Tdynamic _ -> "dynamic"
  | Tgeneric s -> s
  | Toption ty -> begin
    match get_node ty with
    | Tnonnull -> "mixed"
    | _ -> "?" ^ print_ty_exn ty
  end
  | Tfun ft ->
    let params = List.map ft.ft_params ~f:print_fun_param_exn in
    let params =
      if get_ft_variadic ft then
        params @ ["..."]
      else
        params
    in
    Printf.sprintf
      "(function(%s): %s)"
      (String.concat ~sep:", " params)
      (print_ty_exn ft.ft_ret)
  | Ttuple { t_required; t_optional; t_extra } ->
    let required = List.map t_required ~f:print_ty_exn in
    let optional = List.map t_optional ~f:print_optional_tuple_field in
    let extra =
      match t_extra with
      | Tvariadic t_variadic ->
        let variadic =
          if is_nothing t_variadic then
            []
          else
            [print_ty_exn t_variadic ^ "..."]
        in
        variadic
      | Tsplat t_splat -> ["..." ^ print_ty_exn t_splat]
    in
    "(" ^ String.concat ~sep:", " (required @ optional @ extra) ^ ")"
  | Tshape
      (Shape_simple
        { s_origin = _; s_unknown_value = shape_kind; s_fields = fdm }) ->
    let fields = List.map (TShapeMap.elements fdm) ~f:print_shape_field_exn in
    let fields =
      if is_nothing shape_kind then
        fields
      else
        let tail =
          match get_node shape_kind with
          | Toption ty when is_nonnull ty -> "..."
          | Tintersection [] -> "..."
          | _ -> print_ty_exn shape_kind ^ "..."
        in
        fields @ [tail]
    in
    Printf.sprintf "shape(%s)" (String.concat ~sep:", " fields)
  | Tshape (Shape_splat { ss_elems }) ->
    let fields = List.map ss_elems ~f:(fun ty -> "..." ^ print_ty_exn ty) in
    Printf.sprintf "shape(%s)" (String.concat ~sep:", " fields)
  | Tnewtype (name, [], _) -> Utils.strip_ns name
  | Tnewtype (name, tyl, _) ->
    Utils.strip_ns name ^ "<" ^ print_tyl_exn tyl ^ ">"
  | Tclass ((_, name), _, []) -> strip_ns name
  | Tclass ((_, name), _, tyl) ->
    Utils.strip_ns name ^ "<" ^ print_tyl_exn tyl ^ ">"
  | Tvec_or_dict (ty1, ty2) ->
    Printf.sprintf "vec_or_dict<%s, %s>" (print_ty_exn ty1) (print_ty_exn ty2)
  | Taccess (ty, id) -> Printf.sprintf "%s::%s" (print_ty_exn ty) (snd id)
  | Tclass_ptr ty -> Printf.sprintf "class<%s>" (print_ty_exn ty)

and print_tyl_exn tyl = String.concat ~sep:", " (List.map tyl ~f:print_ty_exn)

and print_fun_param_exn param =
  match get_fp_mode param with
  | FPinout -> "inout " ^ print_ty_exn param.fp_type
  | _ -> print_ty_exn param.fp_type

and print_shape_field_exn (name, { sft_optional; sft_ty; _ }) =
  Printf.sprintf
    "%s%s => %s"
    (if sft_optional then
      "?"
    else
      "")
    (print_shape_field_name name)
    (print_ty_exn sft_ty)

and print_shape_field_name name =
  let s = Typing_defs.TShapeField.name name in
  match name with
  | Typing_defs.TSFlit_str _ -> "'" ^ s ^ "'"
  | _ -> s

and print_optional_tuple_field ty = "optional " ^ print_ty_exn ty

let print ?(allow_nothing = false) ty =
  try Some (print_ty_exn ~allow_nothing ty) with
  | Non_denotable -> None
