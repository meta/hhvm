(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

type def = {
  def_pos: Pos.t;
  def_rhs_pos: Pos.t;
  def_needs_grouping: bool;
      (** iff `def_needs_grouping`, then we need to surround with parentheses when inlining.
        ```
          $x = 3 + 4;
          foo($x * 5); --inline--> foo((3+4) * 5)
        ```
  *)
  def_deps: String.Set.t;  (** In `$x = 0 + $y`, `$x` depends on `$y` *)
}

type candidate = {
  name: string;
  def: def;
  use_pos: Pos.t;
}

let remove_leading_whitespace ~source_text pos : Pos.t =
  let rec calc_strip_amount offset =
    let ch = Full_fidelity_source_text.get source_text offset in
    if offset = 0 || Char.equal ch '\n' then
      0
    else if Char.is_whitespace ch then
      1 + calc_strip_amount (offset - 1)
    else
      0
  in
  let orig_col_start = snd @@ Pos.line_column pos in
  let strip_amount = calc_strip_amount (Pos.start_offset pos - 1) in
  Pos.set_col_start (orig_col_start - strip_amount) pos

let next_char_is_newline ~source_text pos : bool =
  let offset =
    let (line, col) = Pos.end_line_column pos in
    Full_fidelity_source_text.position_to_offset source_text (line, col + 1)
  in
  let ch = Full_fidelity_source_text.get source_text offset in
  Char.(ch = '\n')

module Var_info : sig
  type t

  val empty : t

  (** Useful for calculating the dependencies of a variable. In `$x = 0 + $y`, `$x` depends on `$y`. *)
  val referenced : t -> String.Set.t

  val to_candidate_opt : t -> selection:Pos.t -> candidate option

  val add_def : t -> name:string -> def:def -> t

  val add_use : t -> name:string -> use_pos:Pos.t -> t

  val mark_ineligible : t -> name:string -> t

  val merge : t -> t -> t
end = struct
  (** The state of a variable: updated as we traverse the tast *)
  type info =
    | Used_never of def
    | Used_once of {
        use_pos: Pos.t;
        def: def;
      }
        (** `Used_once` is the "good" state: we only offer to inline variables that are used exactly once *)
    | Used_undefined of Pos.t
        (**
    `Used_undefined` indicates a use of a variable that is not defined.
    We do *not* traverse the AST in the same order as variable bindings,
    so we may discover later that the variable is indeed defined.
    See [merge] for an example of how `Used_undefined` is used.
    *)
    | Ineligible

  let use_pos_of_info = function
    | Used_never _ -> None
    | Used_once { use_pos; _ } -> Some use_pos
    | Used_undefined use_pos -> Some use_pos
    | Ineligible -> None

  type t = {
    referenced: String.Set.t;
    infos: info String.Map.t;
  }

  let referenced { referenced; _ } = referenced

  let empty = { infos = String.Map.empty; referenced = String.Set.empty }

  (**
  A variable is a candidate iff:
  - The variable is not `Ineligible`
  - It's the only variable used in the selection
  - None of the variable's deps is redefined between the definition and use of the variable.
    Here is a case where we cannot inline `$x` because a dep (`$y`) changes:

    ```
    $y = 3;
    $x = $y; // $x is defined, depends on $y
    $y = 6;
    // inlining $x on the next line would change behavior because $y was redefined between use and def of $x
    foo(  $x  );
      //  ^-^ selection
    ```
  *)
  let to_candidate_opt { infos; _ } ~selection =
    begin
      let vars_in_selection =
        infos
        |> Map.filter ~f:(fun info ->
               match use_pos_of_info info with
               | Some use_pos -> Pos.contains use_pos selection
               | None -> false)
        |> Map.to_alist
      in
      match vars_in_selection with
      | [(name, Used_once { def; use_pos })] ->
        let dep_may_have_changed =
          def.def_deps
          |> Set.exists ~f:(fun dep ->
                 Map.find infos dep
                 |> Option.exists ~f:(fun dep_info ->
                        match use_pos_of_info dep_info with
                        | Some use_pos ->
                          Pos.start_offset use_pos
                          > Pos.end_offset def.def_rhs_pos
                        | None -> true))
        in
        if not dep_may_have_changed then
          Some { name; def; use_pos }
        else
          None
      | _ -> None
    end

  let add_def { referenced; infos } ~name ~def =
    let infos = Map.set infos ~key:name ~data:(Used_never def) in
    { referenced; infos }

  let add_use { referenced; infos } ~name ~use_pos =
    let infos =
      Map.update infos name ~f:(function
          | None -> Used_undefined use_pos
          | Some (Used_never def) -> Used_once { use_pos; def }
          | Some (Used_once _ | Used_undefined _ | Ineligible) -> Ineligible)
    in
    let referenced = Set.add referenced name in
    { referenced; infos }

  let mark_ineligible { referenced; infos } ~name =
    let infos = Map.set infos ~key:name ~data:Ineligible in
    { referenced; infos }

  let merge
      { referenced = _; infos = infos1 } { referenced = _; infos = infos2 } =
    let infos =
      Map.merge_skewed infos1 infos2 ~combine:(fun ~key:_ v1 v2 ->
          match (v1, v2) with
          | (Used_never def, Used_undefined use_pos) ->
            (*
              This branch is hit for code like the following:
              $x = 0;       // $x is `Used_never` here
              $_ = $x + 1;  // $x is `Used_undefined` here
            *)
            Used_once { def; use_pos }
          | (_, Used_never _) ->
            (* redefined *)
            v2
          | ( (Used_never _ | Used_once _ | Used_undefined _ | Ineligible),
              (Used_once _ | Used_undefined _ | Ineligible) ) ->
            Ineligible)
    in
    { referenced = String.Set.empty; infos }
end

let visitor ~(selection : Pos.t) =
  let in_lvalue = ref false in

  let with_in_lvalue f =
    let orig_in_lvalue = !in_lvalue in
    in_lvalue := true;
    let res = f () in
    in_lvalue := orig_in_lvalue;
    res
  in

  object (self)
    inherit [Var_info.t] Tast_visitor.reduce as super

    method zero = Var_info.empty

    method plus = Var_info.merge

    method! on_class_ env class_ =
      if Pos.contains class_.Aast.c_span selection then
        super#on_class_ env class_
      else
        Var_info.empty

    method! on_fun_ env fun_ =
      if Pos.contains fun_.Aast.f_span selection then
        super#on_fun_ env fun_
      else
        Var_info.empty

    method! on_fun_param env param =
      super#on_fun_param env param
      |> Var_info.mark_ineligible ~name:param.Aast.param_name

    method! on_stmt env stmt =
      let (pos, stmt_) = stmt in
      let open Aast in
      match stmt_ with
      | Expr (_, _, Assign (((_, _, Lvar (_, lid)) as lhs), None, rhs)) ->
        let name = Local_id.get_name lid in
        let def_needs_grouping =
          match rhs with
          | (_, _, (Binop _ | Pipe _ | Eif _)) -> true
          | _ -> false
        in
        let rhs_acc = self#on_expr env rhs in
        let def =
          {
            def_pos = pos;
            def_rhs_pos = Tuple3.get2 rhs;
            def_needs_grouping;
            def_deps = Var_info.referenced rhs_acc;
          }
        in
        let acc = Var_info.add_def ~name ~def rhs_acc in
        self#plus acc (with_in_lvalue (fun () -> self#on_expr env lhs))
      | _ -> super#on_stmt env stmt

    method! on_expr env expr =
      let open Aast in
      match Tuple3.get3 expr with
      | Assign ((_, _, Lvar (_, lid)), Some _, _) ->
        super#on_expr env expr
        |> Var_info.mark_ineligible ~name:(Local_id.get_name lid)
      | Lvar (use_pos, lid) when not !in_lvalue ->
        let name = Local_id.get_name lid in
        Var_info.add_use Var_info.empty ~name ~use_pos
      | _ -> super#on_expr env expr
  end

let start_of_next_line source_text path pos : Pos.t =
  let pos_end =
    let (orig_end_line, _) = Pos.end_line_column pos in
    let end_line = orig_end_line + 1 in
    let end_line_offset =
      Full_fidelity_source_text.position_to_offset source_text (end_line, 0)
    in
    (end_line, end_line_offset, end_line_offset)
  in
  Pos.make_from_lnum_bol_offset
    ~pos_file:path
    ~pos_start:(Pos.line_beg_offset pos)
    ~pos_end

let edits_of_candidate ~path ~source_text { def; use_pos; _ } :
    Code_action_types.edits =
  let change_replace_def =
    let pos = remove_leading_whitespace ~source_text def.def_pos in
    let pos =
      if next_char_is_newline ~source_text def.def_pos then
        start_of_next_line source_text path pos
      else
        pos
    in
    Code_action_types.{ pos; text = "" }
  in
  let change_replace_use =
    let text =
      Full_fidelity_source_text.sub_of_pos source_text def.def_rhs_pos
    in
    let text =
      if def.def_needs_grouping then
        Printf.sprintf "(%s)" text
      else
        text
    in
    Code_action_types.{ pos = use_pos; text }
  in

  Relative_path.Map.singleton path [change_replace_def; change_replace_use]

let refactor_of_candidate ~path ~source_text candidate =
  let edits = lazy (edits_of_candidate ~path ~source_text candidate) in
  Code_action_types.(
    Refactor
      {
        title = Printf.sprintf "Inline variable %s" candidate.name;
        edits;
        selection = None;
        trigger_inline_suggest = false;
      })

let find ~entry selection ctx =
  let source_text = Ast_provider.compute_source_text ~entry in
  let { Tast_provider.Compute_tast.tast; _ } =
    Tast_provider.compute_tast_quarantined ~ctx ~entry
  in
  let path = entry.Provider_context.path in
  (visitor ~selection)#go ctx tast.Tast_with_dynamic.under_normal_assumptions
  |> Var_info.to_candidate_opt ~selection
  |> Option.map ~f:(refactor_of_candidate ~path ~source_text)
  |> Option.to_list
