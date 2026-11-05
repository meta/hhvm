(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

let go (results : Server_lint_types.result) output_json error_format =
  if output_json then
    Server_lint_types.output_json stdout results
  else (
    if not (List.is_empty results) then
      Server_local_config_qe.log_agent_error_format_exposure ();
    Server_lint_types.output_text stdout results error_format
  )
