(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

module Candidate : sig
  (**
In `foo(param_a, param_b, param_c)`
                         ^
                         |
                        selection

  `positions` is the positions of each of the params
  `insertion_index` is 2 (in bounds by construction)
  `pos` is the span from the start of `param_a` to the end of `param_c`
 *)
  type t = private {
    positions: Pos.t list;
    insertion_index: int;
    pos: Pos.t;
  }

  val create_exn : positions:Pos.t list -> insertion_index:int -> t
end = struct
  type t = {
    positions: Pos.t list;
    insertion_index: int;
    pos: Pos.t;
  }

  let create_exn ~positions ~insertion_index =
    if insertion_index >= List.length positions then begin
      Hack_event_logger.invariant_violation_bug
        ~data:
          (Printf.sprintf
             "insertion index: %d positions length: %d"
             insertion_index
             (List.length positions))
        "flip around comma: insertion index out of bounds";
      failwith "flip around comma: insertion index out of bounds"
    end else
      let pos =
        let first = List.hd_exn positions in
        let last = List.last_exn positions in
        Pos.merge first last
      in
      { insertion_index; positions; pos }
end

let list_flip ~insertion_index l =
  let rec aux i = function
    | h1 :: h2 :: tail when i = insertion_index - 1 -> h2 :: h1 :: tail
    | h :: tail -> h :: aux (i + 1) tail
    | [] -> []
  in
  aux 0 l

let find_insertion_index ~(cursor : Pos.t) (positions : Pos.t list) : int option
    =
  let is_after_cursor pos = Pos.start_offset pos > Pos.start_offset cursor in
  let is_before_cursor pos = Pos.start_offset pos < Pos.start_offset cursor in
  if positions |> List.exists ~f:is_before_cursor then
    positions |> List.findi ~f:(fun _i -> is_after_cursor) |> Option.map ~f:fst
  else
    None

let find_candidate ~(cursor : Pos.t) (positions : Pos.t list) :
    Candidate.t option =
  find_insertion_index ~cursor positions
  |> Option.map ~f:(fun insertion_index ->
         Candidate.create_exn ~insertion_index ~positions)

let pos_of_expr = Tuple3.get2

let pos_of_shape_field_name =
  Ast_defs.(
    function
    | SFlit_str (pos, _) -> pos
    | SFclassname (pos, _) -> pos
    | SFclass_const ((class_pos, _), (const_pos, _)) ->
      Pos.merge class_pos const_pos)

let pos_of_type_hint : 'a Aast_defs.type_hint -> Pos.t = function
  | (_, Some (pos, _)) -> pos
  | (_, None) -> Pos.none

let option_or_thunk opt ~f =
  match opt with
  | Some _ -> opt
  | None -> f ()

let pos_of_named_modifier positioned_tree param_pos =
  let module Syn = Full_fidelity_positioned_syntax in
  let root =
    Provider_context.PositionedSyntaxTree.root (Lazy.force positioned_tree)
  in
  Syn.parentage root (Pos.start_offset param_pos)
  |> List.find_map ~f:(fun node ->
         match Syn.syntax node with
         | Syn.ParameterDeclaration { parameter_named; _ }
           when not (Syn.is_missing parameter_named) ->
           Syn.position_exclusive (Pos.filename param_pos) parameter_named
         | _ -> None)

let visitor ~(cursor : Pos.t) ~positioned_tree =
  let find_in_positions = find_candidate ~cursor in
  let find_in_positions_params params =
    let is_easy_to_flip param =
      Aast_defs.(
        (not (Aast_utils.is_param_variadic param))
        && Option.is_none param.param_readonly
        && Option.is_none param.param_visibility
        && (match param.param_callconv with
           | Ast_defs.Pnormal -> true
           | Ast_defs.Pinout _ -> false)
        && List.is_empty param.param_user_attributes)
    in
    let pos_of_expr_opt = function
      | Some (_, pos, _) -> pos
      | _ -> Pos.none
    in
    if List.for_all params ~f:is_easy_to_flip then
      params
      |> List.map
           ~f:
             Aast_defs.(
               fun param ->
                 let named_pos =
                   if Option.is_none param.param_named then
                     Some Pos.none
                   else
                     pos_of_named_modifier positioned_tree param.param_pos
                 in
                 Option.map named_pos ~f:(fun named_pos ->
                     List.fold
                       ~init:param.param_pos
                       ~f:Pos.merge
                       [
                         named_pos;
                         pos_of_expr_opt (Aast_utils.get_param_default param);
                         pos_of_type_hint param.param_type_hint;
                       ]))
      |> Option.all
      |> Option.bind ~f:find_in_positions
    else
      None
  in
  object
    inherit [Candidate.t option] Tast_visitor.reduce as super

    method zero = None

    method plus = Option.first_some

    method! on_method_ env meth =
      if Pos.contains meth.Aast_defs.m_span cursor then
        option_or_thunk (super#on_method_ env meth) ~f:(fun () ->
            find_in_positions_params meth.Aast_defs.m_params)
      else
        None

    method! on_class_ env class_ =
      if Pos.contains class_.Aast_defs.c_span cursor then
        super#on_class_ env class_
      else
        None

    method! on_fun_ env fun_ =
      if Pos.contains fun_.Aast_defs.f_span cursor then
        option_or_thunk (super#on_fun_ env fun_) ~f:(fun () ->
            find_in_positions_params fun_.Aast_defs.f_params)
      else
        None

    method! on_expr_ env expr =
      option_or_thunk (super#on_expr_ env expr) ~f:(fun () ->
          match expr with
          | Aast_defs.(Call { args; _ }) ->
            args
            |> List.map
                 ~f:
                   Aast_defs.(
                     function
                     | Ainout (inout_pos, expr) ->
                       Pos.merge inout_pos (pos_of_expr expr)
                     | Anormal expr -> pos_of_expr expr
                     | Anamed ((name_pos, _), expr) ->
                       Pos.merge name_pos (pos_of_expr expr))
            |> find_in_positions
          | Aast_defs.ValCollection (_, _, exprs)
          | Aast_defs.List exprs
          | Aast_defs.Tuple exprs ->
            exprs |> List.map ~f:pos_of_expr |> find_in_positions
          | Aast_defs.KeyValCollection (_, _, fields) ->
            fields
            |> List.map ~f:(fun (e1, e2) ->
                   Pos.merge (pos_of_expr e1) (pos_of_expr e2))
            |> find_in_positions
          | Aast_defs.Shape fields ->
            if
              List.exists fields ~f:(function
                  | Aast_defs.SF_splat _ -> true
                  | Aast_defs.SF_field _ -> false)
            then
              None
            else
              List.filter_map fields ~f:(function
                  | Aast_defs.SF_field (field_name, expr) ->
                    Some
                      (Pos.merge
                         (pos_of_shape_field_name field_name)
                         (pos_of_expr expr))
                  | Aast_defs.SF_splat _ -> None)
              |> find_in_positions
          | _ -> None)
  end

let edit_of_candidate
    ~path ~source_text Candidate.{ insertion_index; positions; pos } :
    Code_action_types.edits =
  let text =
    positions
    |> List.map ~f:(Full_fidelity_source_text.sub_of_pos source_text)
    |> list_flip ~insertion_index
    |> String.concat ~sep:", "
  in
  let change = Code_action_types.{ pos; text } in
  Relative_path.Map.singleton path [change]

let refactor_of_candidate ~path ~source_text candidate =
  let edits = lazy (edit_of_candidate ~path ~source_text candidate) in
  Code_action_types.(
    Refactor
      {
        title = "Flip around comma";
        edits;
        selection = None;
        trigger_inline_suggest = false;
      })

let find ~entry pos ctx =
  if Pos.length pos = 0 then
    let source_text = Ast_provider.compute_source_text ~entry in
    let { Tast_provider.Compute_tast.tast; _ } =
      Tast_provider.compute_tast_quarantined ~ctx ~entry
    in
    let path = entry.Provider_context.path in
    let positioned_tree = lazy (Ast_provider.compute_cst ~ctx ~entry) in
    (visitor ~cursor:pos ~positioned_tree)#go
      ctx
      tast.Tast_with_dynamic.under_normal_assumptions
    |> Option.map ~f:(refactor_of_candidate ~path ~source_text)
    |> Option.to_list
  else
    []
