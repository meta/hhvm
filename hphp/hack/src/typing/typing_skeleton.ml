(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Generate method skeletons based on the known type.*)

open Hh_prelude
open Typing_defs

(** We keep track of the skeleton in two parts: a prefix and a suffix.

The cursor is intended to be moved in-between the prefix and suffix after
inserting the skeleton. *)
type t = {
  prefix: string;
  suffix: string option;
}

let to_string { prefix; suffix } =
  match suffix with
  | None -> prefix
  | Some suffix -> prefix ^ suffix

let add_suffix s t =
  let suffix = Option.value ~default:"" t.suffix ^ s in
  { t with suffix = Some suffix }

let strip_suffix s t =
  let suffix =
    Option.map t.suffix ~f:(fun suffix ->
        if String.is_suffix ~suffix:s suffix then
          String.sub suffix ~pos:0 ~len:(String.length suffix - String.length s)
        else
          suffix)
  in
  { t with suffix }

let add_prefix s t =
  let prefix = s ^ t.prefix in
  { t with prefix }

let cursor_after_insert pos { prefix; suffix = _ } =
  Pos.advance_string prefix pos |> Pos.shrink_to_end

(* Given a type declaration, return user-denotable syntax that
   represents it. This is lossy: we use mixed if there's no better
   syntax. *)
let rec strip_supportdyn ty =
  match get_node ty with
  | Tapply (name, [ty])
    when String.equal (snd name) Naming_special_names.Classes.cSupportDyn ->
    strip_supportdyn ty
  | _ -> ty

let rec of_decl_ty (ty : decl_ty) : string =
  (* TODO: when we start supporting NoAutoDynamic, revisit this *)
  let ty = strip_supportdyn ty in
  match get_node ty with
  | Tprim p -> Aast_defs.string_of_tprim p
  | Tmixed
  | Twildcard
  | Tany _ ->
    "mixed"
  | Tnonnull -> "nonnull"
  | Tdynamic _ -> "dynamic"
  | Tthis -> "this"
  | Toption ty ->
    (* TODO: when we start supporting NoAutoDynamic, revisit this *)
    (match get_node (strip_supportdyn ty) with
    | Typing_defs.Tnonnull -> "mixed"
    | _ -> "?" ^ of_decl_ty ty)
  | Tfun f ->
    let params = List.map f.ft_params ~f:of_fun_param in
    let params =
      if get_ft_variadic f then
        params @ ["..."]
      else
        params
    in
    Printf.sprintf
      "(function(%s): %s)"
      (String.concat ~sep:", " params)
      (of_decl_ty f.ft_ret)
  | Tgeneric name -> Utils.strip_all_ns name
  | Tapply ((_, name), args) ->
    let name = Utils.strip_all_ns name in
    (match args with
    | [] -> name
    | args ->
      let args = List.map args ~f:of_decl_ty in
      Printf.sprintf "%s<%s>" name (String.concat ~sep:", " args))
  (* TODO akenn: open tuples *)
  | Ttuple { t_required; t_optional = _; t_extra = _ } ->
    let args = List.map t_required ~f:of_decl_ty in
    Printf.sprintf "(%s)" (String.concat ~sep:", " args)
  | Tshape
      (Shape_simple { s_origin = _; s_fields = fields; s_unknown_value = kind })
    ->
    let fields =
      TShapeMap.fold (fun key ty acc -> of_shape_field key ty :: acc) fields []
    in
    let fields_with_ellipsis =
      if is_nothing kind (* Closed shape *) then
        fields
      else
        let kind = of_decl_ty kind in
        let tail =
          if String.equal kind "mixed" then
            "..."
          else
            kind ^ "..."
        in
        fields @ [tail]
    in
    Printf.sprintf "shape(%s)" (String.concat ~sep:", " fields_with_ellipsis)
  | Tshape (Shape_splat { ss_elems }) ->
    let fields = List.map ss_elems ~f:(fun ty -> "..." ^ of_decl_ty ty) in
    Printf.sprintf "shape(%s)" (String.concat ~sep:", " fields)
  | Tunion [] -> "nothing"
  | Tunion _ -> "mixed"
  | Tintersection _ -> "mixed"
  | Tvec_or_dict (key_ty, val_ty) ->
    Printf.sprintf "vec_or_dict<%s, %s>" (of_decl_ty key_ty) (of_decl_ty val_ty)
  | Tlike ty -> of_decl_ty ty
  | Taccess (ty, (_, name)) -> Printf.sprintf "%s::%s" (of_decl_ty ty) name
  | Trefinement (ty, rs) ->
    Printf.sprintf
      "%s with %s"
      (of_decl_ty ty)
      (Class_refinement.to_string of_decl_ty rs)
  | Tclass_ptr ty -> Printf.sprintf "class<%s>" (of_decl_ty ty)

and of_fun_param fp : string = of_decl_ty fp.fp_type

and of_shape_field (name : tshape_field_name) sft : string =
  let name_s =
    match name with
    | TSFregex_group (_, s) -> s
    | TSFlit_str (_, s) -> Printf.sprintf "\"%s\"" s
    | TSFclass_const ((_, c_name), (_, const_name)) ->
      Printf.sprintf "%s::%s" c_name const_name
  in
  let name_s =
    if sft.sft_optional then
      "?" ^ name_s
    else
      name_s
  in
  Printf.sprintf "%s => %s" name_s (of_decl_ty sft.sft_ty)

let param_source (param : decl_ty fun_param) ~(variadic : bool) : string =
  let name = Option.value param.fp_name ~default:"$_" in
  let ty_s = of_decl_ty param.fp_type in
  Printf.sprintf
    "%s %s%s"
    ty_s
    (if variadic then
      "..."
    else
      "")
    name

(* Is this type of the form Awaitable<something> ? *)
let rec is_awaitable (ty : decl_ty) : bool =
  match get_node ty with
  | Tapply ((_, name), _)
    when String.equal name Naming_special_names.Classes.cAwaitable ->
    true
  | Tlike ty -> is_awaitable ty
  | _ -> false

let params_source ~variadic (params : decl_ty fun_params) : string =
  let n = List.length params in
  let explicit_params =
    List.mapi params ~f:(fun i p ->
        param_source ~variadic:(variadic && i + 1 = n) p)
  in
  String.concat ~sep:", " explicit_params

let of_implicit_params (impl_params : decl_ty fun_implicit_params) : string =
  match impl_params.capability with
  | CapDefaults _ -> ""
  | CapTy t ->
    (match get_node t with
    | Tintersection tys ->
      let contexts = List.map tys ~f:of_decl_ty in
      Printf.sprintf "[%s]" (String.concat ~sep:", " contexts)
    | _ -> "")

let of_method
    (name : string) (meth : class_elt) ~is_static ~is_override ~open_braces : t
    =
  (* TODO: when we start supporting NoAutoDynamic, revisit this *)
  let (_, ty_) = deref (strip_supportdyn (Lazy.force meth.ce_type)) in
  let (params, return_ty, async_modifier, capabilities) =
    match ty_ with
    | Tfun ft ->
      ( params_source ~variadic:(get_ft_variadic ft) ft.ft_params,
        of_decl_ty ft.ft_ret,
        (if is_awaitable ft.ft_ret then
          "async "
        else
          ""),
        of_implicit_params ft.ft_implicit_params )
    | _ -> ("", "mixed", "", "")
  in

  let prefix =
    Printf.sprintf
      "\n%s  %s %s%sfunction %s(%s)%s: %s {"
      (if is_override then
        "  <<__Override>>\n"
      else
        "")
      (Typing_defs.string_of_visibility meth.ce_visibility)
      (if is_static then
        "static "
      else
        "")
      async_modifier
      name
      params
      capabilities
      return_ty
  in
  let prefix =
    if open_braces then
      prefix ^ "\n    "
    else
      prefix
  in
  let suffix = "}\n" in
  let suffix =
    if open_braces then
      "\n  " ^ suffix
    else
      suffix
  in
  { prefix; suffix = Some suffix }
