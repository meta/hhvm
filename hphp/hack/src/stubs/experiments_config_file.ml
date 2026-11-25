(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

let update
    ~(silent : bool) ~(file : string) ~(source : string option) ~(ttl : float) :
    (string, string) result =
  ignore (silent, file, source, ttl);
  Ok "Experiments config update: nothing to do"
