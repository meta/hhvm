(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type pos = {
  start: int;
  length: int;
}
[@@deriving ord]

let buf = Buffer.create 1024

let hint_to_string_and_symbols ~is_ctx (hint : Aast.hint) =
  Buffer.reset buf;
  let xrefs = ref [] in
  let append ?annot str =
    let length = String.length str in
    let pos = { start = Buffer.length buf; length } in
    Buffer.add_string buf str;
    Option.iter annot ~f:(fun file_pos -> xrefs := (file_pos, pos) :: !xrefs)
  in
  let parse_gen_seq ~sep ~f = function
    | [] -> ()
    | [x] -> f x
    | x :: xs ->
      f x;
      List.iter xs ~f:(fun x ->
          append sep;
          f x)
  in
  let rec parse ~is_ctx t =
    let open Aast in
    match snd t with
    | Hmixed -> append "mixed"
    | Hwildcard -> append "_"
    | Hnonnull -> append "nonnull"
    | Hthis -> append "this"
    | Hdynamic -> append "dynamic"
    | Hnothing -> append "nothing"
    | Hoption hint ->
      append "?";
      parse ~is_ctx hint
    | Hlike hint ->
      append "~";
      parse ~is_ctx hint
    | Hsoft hint ->
      append "@";
      parse ~is_ctx hint
    | Hvar v -> append v
    | Habstr name -> append (Typing_print.strip_ns name)
    | Happly ((_, "\\HH\\supportdyn"), [hint]) -> parse ~is_ctx hint
    | Happly ((_, "\\HH\\string"), _) -> append "string"
    | Happly ((file_pos, cn), []) ->
      append ~annot:file_pos (Typing_print.strip_ns cn)
    | Happly ((file_pos, cn), hints) ->
      append ~annot:file_pos (Typing_print.strip_ns cn);
      append "<";
      parse_gen_seq ~sep:", " ~f:(parse ~is_ctx) hints;
      append ">"
    (* TODO optional and variadic components T201398626 T201398652 *)
    | Htuple { tup_required; tup_extra = _ } ->
      append "(";
      parse_gen_seq ~sep:", " ~f:(parse ~is_ctx) tup_required;
      append ")"
    | Hprim p -> append (Aast_defs.string_of_tprim p)
    | Haccess (hint, sids) ->
      parse ~is_ctx hint;
      List.iter sids ~f:(fun (file_pos, sid) ->
          append "::";
          append ~annot:file_pos sid)
    | Hfun
        {
          hf_param_tys;
          hf_param_info;
          hf_variadic_ty;
          hf_named_variadic_ty = _;
          hf_return_ty;
          hf_ctxs;
          _;
        } ->
      append "(function(";
      parse_fun hf_param_tys hf_param_info hf_variadic_ty;
      append ")";
      Option.iter hf_ctxs ~f:(fun (_, ctxs) ->
          append "[";
          parse_gen_seq ~sep:", " ~f:(parse ~is_ctx:true) ctxs;
          append "]");
      append ": ";
      parse ~is_ctx hf_return_ty;
      append ")"
    | Hfun_context name ->
      append "ctx ";
      append (Typing_print.strip_ns name)
    | Hclass_ptr (kind, hint) ->
      (match kind with
      | CKclass -> append "class<"
      | CKenum -> append "enum<");
      parse ~is_ctx hint;
      append ">"
    | Hshape
        { nsi_allows_unknown_fields; nsi_field_map; nsi_unknown_fields_type } ->
      append "shape(";
      parse_shape
        nsi_allows_unknown_fields
        nsi_field_map
        nsi_unknown_fields_type;
      append ")"
    | Hrefinement (hint, members) ->
      parse ~is_ctx:false hint;
      append " with { ";
      parse_gen_seq ~sep:"; " ~f:parse_member members;
      append " }"
    | Hvec_or_dict (None, vhint) ->
      append "vec_or_dict";
      append "<";
      parse ~is_ctx vhint;
      append ">"
    | Hvec_or_dict (Some khint, vhint) ->
      append "vec_or_dict";
      append "<";
      parse ~is_ctx khint;
      append ", ";
      parse ~is_ctx vhint;
      append ">"
    | Hunion hints ->
      append "(";
      parse_gen_seq ~sep:"|" ~f:(parse ~is_ctx) hints;
      append ")"
    | Hintersection hints when is_ctx ->
      append "[";
      parse_gen_seq ~sep:", " ~f:(parse ~is_ctx) hints;
      append "]"
    | Hintersection hints ->
      append "(";
      parse_gen_seq ~sep:"&" ~f:(parse ~is_ctx) hints;
      append ")"
  and parse_bound ~is_ctx (kind, hint) =
    let constraint_ =
      match kind with
      | `E -> "= "
      | `L -> "super "
      | `U -> "as "
    in
    append constraint_;
    parse ~is_ctx hint
  and parse_member = function
    | Aast.Rtype (ident, ref) ->
      append "type ";
      append (snd ident);
      append " ";
      let bounds =
        match ref with
        | Aast.TRexact hint -> [(`E, hint)]
        | Aast.TRloose { Aast.tr_lower; tr_upper } ->
          List.map tr_lower ~f:(fun x -> (`L, x))
          @ List.map tr_upper ~f:(fun x -> (`U, x))
      in
      parse_gen_seq ~sep:" " ~f:(parse_bound ~is_ctx:false) bounds
    | Aast.Rctx (ident, ref) ->
      append "ctx ";
      append (snd ident);
      append " ";
      let bounds =
        match ref with
        | Aast.CRexact hint -> [(`E, hint)]
        | Aast.CRloose { Aast.cr_lower; cr_upper } ->
          let opt_map = Option.value_map ~default:[] in
          opt_map cr_lower ~f:(fun x -> [(`L, x)])
          @ opt_map cr_upper ~f:(fun x -> [(`U, x)])
      in
      parse_gen_seq ~sep:" " ~f:(parse_bound ~is_ctx:true) bounds
  and parse_shape open_ nsi_field_map unknown_fields_type =
    parse_gen_seq
      ~sep:", "
      ~f:(function
        | Aast.SE_field sfi -> parse_shape_field sfi
        | Aast.SE_splat h ->
          append "...";
          parse ~is_ctx:false h)
      nsi_field_map;
    match unknown_fields_type with
    | Some h ->
      if not (List.is_empty nsi_field_map) then append ", ";
      parse ~is_ctx:false h;
      append "..."
    | None ->
      if open_ then begin
        if not (List.is_empty nsi_field_map) then append ", ";
        append "..."
      end
  and parse_shape_field Aast.{ sfi_optional; sfi_name; sfi_hint } =
    if sfi_optional then append "?";
    parse_shape_field_name sfi_name;
    append " => ";
    parse ~is_ctx:false sfi_hint
  and parse_shape_field_name = function
    | Ast_defs.SFlit_str (_, s) -> append ("'" ^ s ^ "'")
    | Ast_defs.SFclassname (_, c) -> append ("nameof " ^ Typing_print.strip_ns c)
    | Ast_defs.SFclass_const ((pos, c), (_, s)) ->
      append ~annot:pos (Typing_print.strip_ns c);
      append "::";
      append s
  and parse_variadic ~first variadic =
    Option.iter variadic ~f:(fun hint ->
        if not first then append ", ";
        parse ~is_ctx:false hint;
        append "...")
  and parse_fun hf_param_tys hf_param_info variadic =
    let hf_param_modifiers =
      List.map hf_param_info ~f:(fun i ->
          Option.value_map i ~default:(None, false, false, false) ~f:(fun i ->
              let inout =
                match i.Aast.hfparam_kind with
                | Ast_defs.Pnormal -> None
                | Ast_defs.Pinout p -> Some (Ast_defs.Pinout p)
              in
              let readonly = Option.is_some i.Aast.hfparam_readonlyness in
              let named = Option.is_some i.Aast.hfparam_named in
              let optional = Option.is_some i.Aast.hfparam_optional in
              (inout, readonly, named, optional)))
    in
    match List.zip_exn hf_param_modifiers hf_param_tys with
    | [] -> parse_variadic ~first:true variadic
    | ats ->
      parse_gen_seq ~sep:", " ~f:parse_annotated_hint ats;
      parse_variadic ~first:false variadic
  and parse_annotated_hint ((inout, readonly, named, optional), hint) =
    if named then append "named ";
    if optional then append "optional ";
    Option.iter inout ~f:(fun _ -> append "inout ");
    if readonly then append "readonly ";
    parse ~is_ctx:false hint
  in
  parse ~is_ctx hint;
  (Buffer.contents buf, !xrefs)

let hint_to_string ~is_ctx (hint : Aast.hint) =
  hint_to_string_and_symbols ~is_ctx hint |> fst

let expr_to_string source_text (_, pos, _) =
  let strip_nested_quotes str =
    let len = String.length str in
    let firstc = str.[0] in
    let lastc = str.[len - 1] in
    if
      len >= 2
      && ((Char.equal '"' firstc && Char.equal '"' lastc)
         || (Char.equal '\'' firstc && Char.equal '\'' lastc))
    then
      String.sub str ~pos:1 ~len:(len - 2)
    else
      str
  in
  (* Replace any codepoints that are not valid UTF-8 with
     the unrepresentable character. *)
  let check_utf8 str =
    let b = Buffer.create (String.length str) in
    let replace_malformed () _index = function
      | `Uchar u -> Uutf.Buffer.add_utf_8 b u
      | `Malformed _ -> Uutf.Buffer.add_utf_8 b Uutf.u_rep
    in
    Uutf.String.fold_utf_8 replace_malformed () str;
    Buffer.contents b
  in
  let source_at_span source_text pos =
    let st = Pos.start_offset pos in
    let fi = Pos.end_offset pos in
    let source_text = Full_fidelity_source_text.sub source_text st (fi - st) in
    check_utf8 source_text
  in
  source_at_span source_text pos |> strip_nested_quotes

let strip_tparams name =
  match String.index name '<' with
  | None -> name
  | Some i -> String.sub name ~pos:0 ~len:i

let rec hint_to_angle h =
  let open Aast in
  let open Hack in
  match snd h with
  | Hlike h -> Hint.(Key (Like (hint_to_angle h)))
  | Hsoft h -> Hint.(Key (Like (hint_to_angle h)))
  | Hvar h -> Hint.(Key (Var_ h))
  (* TODO splat fields T203492030 *)
  | Htuple { tup_required; tup_extra = Hextra { tup_optional; tup_variadic } }
    ->
    let req = List.map tup_required ~f:hint_to_angle in
    let opt = List.map tup_optional ~f:hint_to_angle in
    let variadic = Option.map tup_variadic ~f:hint_to_angle in
    Hint.(Key (Tuple { req; opt; variadic }))
  | Hprim t -> Hint.(Key (Prim (Type.Key (Aast_defs.string_of_tprim t))))
  | Happly ((_, cn), _)
    when String.equal Naming_special_names.Classes.cString cn ->
    Hint.(Key (Prim (Type.Key "string")))
  (* TODO(T199610905) update glean schema *)
  | Hclass_ptr (_kind, hint) -> Hint.(Key (Class_args (hint_to_angle hint)))
  | Hfun_context c -> Hint.(Key (Fun_context c))
  | Hvec_or_dict (maybe_k, v) ->
    Hint.(
      Key
        (Vect_or_dict
           {
             maybe_key = Option.map ~f:hint_to_angle maybe_k;
             value_ = hint_to_angle v;
           }))
  | Hintersection hints ->
    let values = List.map hints ~f:hint_to_angle in
    Hint.(Key (Intersection values))
  | Hunion hints ->
    let values = List.map hints ~f:hint_to_angle in
    Hint.(Key (Union_ values))
  | Hoption h -> Hint.(Key (Option (hint_to_angle h)))
  | Hshape { nsi_allows_unknown_fields; nsi_field_map; nsi_unknown_fields_type }
    ->
    let shape_field_name_to_hint = function
      | Ast_defs.SFlit_str (_, s) -> ShapeKV.Sf_lit_string s
      | Ast_defs.SFclassname (_, c) ->
        ShapeKV.Sf_class_const
          FieldClassConst.
            { container = Util.make_qname c; name = Name.Key "class" }
      | Ast_defs.SFclass_const ((_pos, c), (_, s)) ->
        ShapeKV.Sf_class_const
          FieldClassConst.{ container = Util.make_qname c; name = Name.Key s }
    in
    let f Aast_defs.{ sfi_optional; sfi_name; sfi_hint } =
      ShapeKV.
        {
          key = shape_field_name_to_hint sfi_name;
          value = hint_to_angle sfi_hint;
          opt = sfi_optional;
        }
    in
    let fields =
      List.map nsi_field_map ~f:(function
          | Aast_defs.SE_field sfi -> Some (f sfi)
          | Aast_defs.SE_splat _ -> None)
      |> Option.all
    in
    (match (nsi_unknown_fields_type, fields) with
    | (None, Some map_) ->
      Hint.(Key (Shape { open_ = nsi_allows_unknown_fields; map_ }))
    | (Some _, _)
    | (_, None) ->
      Hint.(Key (Other (Type.Key (hint_to_string ~is_ctx:false h)))))
  | Happly ((_file_pos, cn), hints) ->
    let values = List.map hints ~f:hint_to_angle in
    Hint.(Key (Apply { class_name = Util.make_qname cn; values }))
  | Hmixed -> Hint.(Key (Mixed ()))
  | Hwildcard -> Hint.(Key (Wildcard ()))
  | Hnonnull -> Hint.(Key (Nonnull ()))
  | Hdynamic -> Hint.(Key (Dynamic ()))
  | Hnothing -> Hint.(Key (Nothing ()))
  | Hthis -> Hint.(Key (This_ ()))
  | Hrefinement _
  | Hfun _
  | Htuple _
  | Haccess (_, _)
  | Habstr _ ->
    Hint.(Key (Other (Type.Key (hint_to_string ~is_ctx:false h))))
