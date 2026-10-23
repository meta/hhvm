(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs

type pos = Relative_path.t * File_content.Position.t

type type_spec =
  | TSpos of pos
  | TSjson of Yojson.Safe.t

type query =
  | PosJson of pos * Yojson.Safe.t
  | JsonPos of Yojson.Safe.t * pos
  | JsonJson of Yojson.Safe.t * Yojson.Safe.t

let expand_path file =
  let path = Path.make file in
  if Path.file_exists path then
    Ok (Path.to_string path)
  else
    let file = Filename.concat (Sys.getcwd ()) file in
    let path = Path.make file in
    if Path.file_exists path then
      Ok (Path.to_string path)
    else
      Error (Printf.sprintf "File not found: %s" file)

let get_type_at_pos ctx tast_map pos :
    (Typing_env_types.env * locl_ty, string) result =
  let (path, pos) = pos in
  let tast = Relative_path.Map.find tast_map path in
  match Server_infer_type.type_at_pos ctx tast pos with
  | Some info ->
    let Equal = Tast_env.eq_typing_env in
    Ok (Server_infer_type.get_env info, Server_infer_type.get_type info)
  | _ ->
    Error
      (Printf.sprintf
         "Failed to get type for pos %s:%s"
         (Relative_path.to_absolute path)
         (File_content.Position.to_string_one_based pos))

(* Returns list of error strings *)
let rec validate_free_type env locl_ty =
  match get_node locl_ty with
  (* notably, we don't validate arity of the type arguments
     Extra args appear to be ignored when subtyping and
     missing args only result in "true" if both types are missing that arg *)
  | Tclass ((_, class_id), _exact, tyargs) ->
    (match Typing_env.get_class env class_id with
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      ["Unbound class name " ^ class_id]
    | Decl_entry.Found _ -> validate_l env tyargs)
  | Ttuple { t_required; t_optional; t_extra } ->
    validate_l env t_required
    @ validate_l env t_optional
    @ begin
        match t_extra with
        | Tvariadic t_variadic -> validate_free_type env t_variadic
        | Tsplat t_splat -> validate_free_type env t_splat
      end
  | Tunion tyargs
  | Tintersection tyargs ->
    validate_l env tyargs
  | Toption locl_ty -> validate_free_type env locl_ty
  | Tnonnull
  | Tdynamic _
  | Tprim _
  | Tany _ ->
    []
  | Tvec_or_dict (kty, vty) ->
    validate_free_type env kty @ validate_free_type env vty
  | Tfun tfun ->
    validate_l env (List.map tfun.ft_params ~f:(fun fp -> fp.fp_type))
    @ validate_free_type env tfun.ft_ret
  | Tshape (Shape_simple { s_origin = _origin; s_unknown_value; s_fields = fm })
    ->
    let field_tys =
      List.map (TShapeMap.values fm) ~f:(fun field -> field.sft_ty)
    in
    validate_free_type env s_unknown_value @ validate_l env field_tys
  | Tshape (Shape_splat { ss_elems }) -> validate_l env ss_elems
  | Tnewtype (_name, tyargs, _) -> validate_l env tyargs
  | Tclass_ptr ty -> validate_free_type env ty
  (* These aren't even created by Typing_json.to_locl_ty *)
  | Tneg _
  | Tvar _
  | Taccess _
  | Tdependent _
  (* Unsupported b/c relative/erroneous *)
  | Tlabel _
  | Tgeneric _ ->
    [
      Printf.sprintf
        "Unsupported free type %s"
        (Typing_print.full ~hide_internals:true env locl_ty);
    ]

and validate_l env locl_tyl =
  List.concat_map locl_tyl ~f:(validate_free_type env)

let get_type_from_json ctx json : (locl_ty, string list) result =
  let locl_ty = Typing_json.to_locl_ty ~keytrace:[] ctx json in
  match locl_ty with
  | Ok locl_ty ->
    let env = Typing_env_types.empty ctx Relative_path.default ~droot:None in
    (match validate_free_type env locl_ty with
    | [] -> Ok locl_ty
    | errl -> Error errl)
  | Error err -> Error [show_deserialization_error err]

let get_type_spec_from_json json : (type_spec, string) result =
  let open Yojson.Safe.Util in
  match json |> member "kind" |> to_string_option with
  | None -> Error "Missing or non-string 'kind' field"
  | Some value ->
    (match value with
    | "type" ->
      (match json |> member "type" with
      | `Null -> Error "Missing 'type' field"
      | type_json -> Ok (TSjson type_json))
    | "pos" ->
      (match json |> member "pos" |> to_string_option with
      | None -> Error "Missing or non-string 'pos' field"
      | Some pos_str ->
        (match String.split ~on:':' pos_str with
        | [file; line; col] ->
          (match expand_path file with
          | Ok file ->
            let path = Relative_path.create_detect_prefix file in
            let pos =
              File_content.Position.from_one_based
                (int_of_string line)
                (int_of_string col)
            in
            Ok (TSpos (path, pos))
          | Error e -> Error e)
        | _ ->
          Error
            (Printf.sprintf
               "Position %s is malformed. Expected file:line:column"
               pos_str)))
    | bad_kind -> Error ("Unexpected kind " ^ bad_kind))

type is_subtype_result = {
  is_subtype: bool;
  ty_left: string;
  ty_right: string;
}

let is_subtype env l_ty r_ty : is_subtype_result =
  {
    is_subtype = Typing_subtype.is_sub_type env l_ty r_ty;
    ty_left = Typing_print.full ~hide_internals:true env l_ty;
    ty_right = Typing_print.full ~hide_internals:true env r_ty;
  }

let helper
    acc
    ctx
    (query_with_path_alist : (int * Relative_path.t option * query) list) :
    (int * (is_subtype_result, string list) result) list =
  let (ctx, tast_map) =
    let paths =
      List.filter_map query_with_path_alist ~f:(fun (_, path_opt, _) ->
          path_opt)
    in
    Server_infer_type_batch.get_tast_map ctx paths
  in
  List.map query_with_path_alist ~f:(fun (i, _path_opt, query) ->
      let result =
        match query with
        | JsonJson (json_l, json_r) ->
          Result.combine
            (get_type_from_json ctx json_l)
            (get_type_from_json ctx json_r)
            ~ok:(fun l r ->
              let env =
                Typing_env_types.empty ctx Relative_path.default ~droot:None
              in
              is_subtype env l r)
            ~err:(fun a b -> a @ b)
        | JsonPos (json, pos) ->
          Result.combine
            (get_type_from_json ctx json)
            (Result.map_error (get_type_at_pos ctx tast_map pos) ~f:(fun e ->
                 [e]))
            ~ok:(fun l (env, r) -> is_subtype env l r)
            ~err:(fun a b -> a @ b)
        | PosJson (pos, json) ->
          Result.combine
            (Result.map_error (get_type_at_pos ctx tast_map pos) ~f:(fun e ->
                 [e]))
            (get_type_from_json ctx json)
            ~ok:(fun (env, l) r -> is_subtype env l r)
            ~err:(fun a b -> a @ b)
      in
      (i, result))
  @ acc

let parallel_helper workers ctx query_with_path_alist :
    (int * (is_subtype_result, string list) result) list =
  let add_query_to_map_if_some_path map (i, path_opt, query) =
    match path_opt with
    | Some path ->
      let entry = (i, path_opt, query) in
      Relative_path.Map.update
        path
        (function
          | None -> Some [entry]
          | Some others -> Some (entry :: others))
        map
    | None -> map
  in
  let (query_with_some_path_alist, query_with_none_path_alist) =
    List.partition_tf query_with_path_alist ~f:(fun (_, path_opt, _) ->
        Option.is_some path_opt)
  in
  let query_with_some_path_alists_by_file =
    List.fold
      ~init:Relative_path.Map.empty
      ~f:add_query_to_map_if_some_path
      query_with_some_path_alist
    |> Relative_path.Map.values
  in
  let query_with_path_alists =
    query_with_some_path_alists_by_file @ [query_with_none_path_alist]
  in
  Multi_worker.call
    workers
    ~job:(fun acc query_with_path_alist ->
      helper acc ctx (List.concat query_with_path_alist))
    ~neutral:[]
    ~merge:List.rev_append
    ~next:(Multi_worker.next workers query_with_path_alists)

let check workers str env =
  let ctx = Provider_utils.ctx_from_server_env env in
  match Yojson.Safe.from_string str with
  | exception Yojson.Json_error message ->
    Error (Printf.sprintf "Invalid JSON input: %s" message)
  | `List json_l ->
    let spec_pair_result_alist =
      List.mapi json_l ~f:(fun i json ->
          let pair =
            match json with
            | `List [json_l; json_r] ->
              let get_type_spec_from_json_with_el json =
                Result.map_error (get_type_spec_from_json json) ~f:(fun e ->
                    [e])
              in
              Result.combine
                (get_type_spec_from_json_with_el json_l)
                (get_type_spec_from_json_with_el json_r)
                ~ok:(fun l r -> (l, r))
                ~err:(fun l r -> l @ r)
            | _ -> Error ["Expected JSON array of size 2"]
          in
          (i, pair))
    in
    let (query_with_path_alist, error_alist) =
      List.partition_map spec_pair_result_alist ~f:(fun (i, x) ->
          match x with
          | Ok (TSpos ((path, _) as pos), TSjson json) ->
            Either.first (i, Some path, PosJson (pos, json))
          | Ok (TSjson json_l, TSjson json_r) ->
            Either.first (i, None, JsonJson (json_l, json_r))
          | Ok (TSjson json, TSpos ((path, _) as pos)) ->
            Either.first (i, Some path, JsonPos (json, pos))
          | Ok (TSpos _, TSpos _) ->
            Either.second
              (i, Error ["Cannot provide a position for the both types"])
          | Error e -> Either.second (i, Error e))
    in
    let num_files =
      query_with_path_alist
      |> List.filter_map ~f:(fun (_, path_opt, _) -> path_opt)
      |> Relative_path.Set.of_list
      |> Relative_path.Set.cardinal
    in
    (* TODO: Should we removes duplicates of the same query?
       Or somehow even avoid multiple type-at-pos lookups for positions
       that appear in multiplle distinct queries? *)
    let result_alist =
      if num_files < 10 then
        helper [] ctx query_with_path_alist
      else
        parallel_helper workers ctx query_with_path_alist
    in
    let result_alist = result_alist @ error_alist in
    let result_map_by_i = Int.Map.of_alist_exn result_alist in
    let result_list =
      List.mapi json_l ~f:(fun i _ -> Map.find_exn result_map_by_i i)
    in
    let json_result_list =
      List.map result_list ~f:(fun res ->
          let (status, value) =
            match res with
            | Ok is_subtype_result ->
              ( "ok",
                `Assoc
                  [
                    ("is_subtype", `Bool is_subtype_result.is_subtype);
                    ("left", `String is_subtype_result.ty_left);
                    ("right", `String is_subtype_result.ty_right);
                  ] )
            | Error l -> ("errors", `List (List.map l ~f:(fun s -> `String s)))
          in
          `Assoc [(status, value)])
    in
    Ok (Yojson.Safe.to_string (`List json_result_list))
  | _ -> Error "Expected JSON array"
