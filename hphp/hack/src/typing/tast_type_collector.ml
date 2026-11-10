(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type collected_type = Tast_env.env * Typing_defs.phase_ty

let make_type_collector () =
  object (self)
    inherit [_] Tast_visitor.reduce as super

    method zero = Pos.AbsolutePosMap.empty

    method plus = Pos.AbsolutePosMap.union ~combine:(fun _ a b -> Some (a @ b))

    method! on_expr env (ty, p, expr_) =
      self#plus
        (Pos.AbsolutePosMap.singleton
           (Pos.to_absolute p)
           [(env, Typing_defs.LoclTy ty)])
        (super#on_expr env (ty, p, expr_))

    method! on_class_id env (ty, _, cid) =
      match cid with
      | Aast.CI (p, _) ->
        Pos.AbsolutePosMap.singleton
          (Pos.to_absolute p)
          [(env, Typing_defs.LoclTy ty)]
      | _ -> Pos.AbsolutePosMap.empty

    method! on_fun_param env fp =
      self#plus
        (Pos.AbsolutePosMap.singleton
           (Pos.to_absolute fp.Aast.param_pos)
           [(env, Typing_defs.LoclTy fp.Aast.param_annotation)])
        (super#on_fun_param env fp)

    method! on_hint (env : Tast_env.t) hint =
      let (pos, _) = hint in
      let ty = Tast_env.hint_to_ty env hint in
      Pos.AbsolutePosMap.singleton
        (Pos.to_absolute pos)
        [(env, Typing_defs.DeclTy ty)]

    (* Coeffects are irrelevant to this collector's consumers (CST search
       and the lambda parameter rewriter), so don't traverse them. *)
    method! on_contexts _env _ctxs = Pos.AbsolutePosMap.empty
  end

let type_collector = lazy (make_type_collector ())

let collect_types tast =
  Diagnostics.ignore_ (fun () -> (Lazy.force type_collector)#go tast)

(*
  Ideally this would be just Pos.AbsolutePosMap.find_opt, however the positions
  in the Tast are off by 1 from positions in the full fidelity parse trees.

  TODO: Fix this when the full fidelity parse tree becomes the parser for type checking.
*)
let get_from_pos_map
    (position : Pos.absolute) (map : collected_type list Pos.AbsolutePosMap.t) =
  let position = Pos.advance_one position in
  Pos.AbsolutePosMap.find_opt position map
