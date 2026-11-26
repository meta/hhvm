(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* This elaboration pass will replace Hsoft(_)` hints, with either
   - the inner hint to which the constructor is applied; OR
   - `Hlike`, when the `interpret_soft_types_as_like_types` typechecker option
     is enabled
*)
val pass : Naming_phase_env.t Naming_phase_pass.t
