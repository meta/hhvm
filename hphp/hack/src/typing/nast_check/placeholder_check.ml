(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

type env = {
  in_lvalue: bool;
  inout_param_pos: Pos.t option;
}

let check_placeholder env pos =
  if not env.in_lvalue then
    Diagnostics.add_diagnostic
      Nast_check_error.(to_user_diagnostic @@ Placeholder_read pos)
  else
    Option.iter env.inout_param_pos ~f:(fun param_pos ->
        Diagnostics.add_diagnostic
          Nast_check_error.(
            to_user_diagnostic @@ Placeholder_inout_write { pos; param_pos }))

let is_placeholder id =
  String.equal
    (Local_id.get_name id)
    Naming_special_names.SpecialIdents.placeholder

let check_binding env (pos, id) =
  if is_placeholder id then check_placeholder { env with in_lvalue = true } pos

let make_visitor () =
  object (self)
    inherit [_] Aast.iter as super

    (* Each callable body is checked by its own handler invocation. *)
    method! on_fun_ _env _f = ()

    method! on_expr env ((_, pos, expr_) as e) =
      match expr_ with
      | Aast.Lplaceholder _ -> check_placeholder env pos
      | Aast.Assign (lhs, None, rhs) ->
        self#on_expr { env with in_lvalue = true } lhs;
        self#on_expr { env with in_lvalue = false } rhs
      | Aast.List _ -> super#on_expr env e
      | _ -> super#on_expr { env with in_lvalue = false } e

    method! on_Foreach env collection binding body =
      self#on_expr { env with in_lvalue = false } collection;
      self#on_as_expr { env with in_lvalue = true } binding;
      self#on_block { env with in_lvalue = false } body

    method! on_catch env ((_, id, _) as catch) =
      check_binding env id;
      super#on_catch env catch

    method! on_DtLvar env id = check_binding env id

    method! on_capture_lid env (_, (pos, id)) =
      if is_placeholder id then
        check_placeholder { env with in_lvalue = false } pos
  end

let visitor = lazy (make_visitor ())

let check_body params body =
  let inout_param_pos =
    List.find_map params ~f:(fun param ->
        match param.Aast.param_callconv with
        | Ast_defs.Pinout _
          when String.equal
                 param.Aast.param_name
                 Naming_special_names.SpecialIdents.placeholder ->
          Some param.Aast.param_pos
        | _ -> None)
  in
  (Lazy.force visitor)#on_func_body { in_lvalue = false; inout_param_pos } body

let handler =
  object
    inherit Nast_visitor.handler_base

    method! at_fun_ _env f = check_body f.Aast.f_params f.Aast.f_body

    method! at_method_ _env m = check_body m.Aast.m_params m.Aast.m_body
  end
