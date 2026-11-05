(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

let error_codes = Typing_warning_utils.codes Typing_warning.Redundant_concurrent

let handler ~as_lint:_ =
  object
    inherit Tast_visitor.handler_base

    method! at_stmt env =
      function
      | (pos, Aast.Concurrent [_]) ->
        Tast_env.add_warning env (pos, Typing_warning.Redundant_concurrent, ())
      | _ -> ()
  end
