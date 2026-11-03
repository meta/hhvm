(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Dependency kinds and their stable hash tags. Tag 4 is reserved.
    Constructor order also determines the OCaml representation. *)
type dep_kind =
  | KGConst [@value 0]
  | KFun [@value 1]
  | KType [@value 2]
  | KExtends [@value 3]
  | KConst [@value 5]
  | KConstructor [@value 6]
  | KProp [@value 7]
  | KSProp [@value 8]
  | KMethod [@value 9]
  | KSMethod [@value 10]
  | KAllMembers [@value 11]
  | KGConstName [@value 12]
  | KModule [@value 13]
  | KDeclares [@value 14]
  | KNotSubtype [@value 15]
  | KFile [@value 16]
[@@deriving enum]
