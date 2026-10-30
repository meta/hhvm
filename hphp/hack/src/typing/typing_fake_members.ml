(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module Reason = Typing_reason

module S = struct
  type t = Local_id.t * Reason.blame

  let compare (lid1, _) (lid2, _) = Local_id.compare lid1 lid2
end

(* A set that treats blame as metadata *)
module BlameSet = struct
  include Stdlib.Set.Make (S)

  let find_opt lid blame_set =
    filter (fun (lid', _) -> Local_id.equal lid lid') blame_set |> choose_opt

  let member lid blame_set =
    exists (fun (lid', _) -> Local_id.equal lid lid') blame_set

  let add lid blame blame_set = add (lid, blame) blame_set

  let remove lid blame_set =
    filter (fun (lid', _) -> not @@ Local_id.equal lid lid') blame_set

  let attach_blame blame blame_set =
    map (fun (lid, _) -> (lid, blame)) blame_set
end

(* A ledger consisting [valid] and [invalid] fake member sets.
 *
 * Those in [invalid] have been invalidated by a call, lambda, assignment, or
 * by going out of scope described by the blame attached to the element, but
 * [valid] are fake members that can still be used in typing. For example
 *
 *   // { valid = []; invalid = [] }
 *   if ($x::f is nonnull) { // P0
 *     // { valid = [("$x::f", Blame_out_of_scope P0)]; invalid = [] }
 *     foo(); // P1
 *     // { valid = []; invalid = [("$x::f", Blame_call P1)] }
 *     if ($y::g is nonnull) { // P2
 *       // { valid = [("$y::g", Blame_out_of_scope P2)]; invalid = [("$x::f", Blame_call P1)] }
 *       $f = () ==> ... // P3
 *       // { valid = []; invalid = [("$x::f", Blame_lambda P3); ("$y::g", Blame_lambda P2)] }
 *       $y::g = 42; // P4
 *       // { valid = [("$y::g", Blame_out_of_scope P4)]; invalid = [("$x::f", Blame_call P1)] }
 * .   } // { valid = []; invalid = [("$y::g", Blame_out_of_scope P4); ("$x::f", Blame_lambda P3)] }
 *)
type t = {
  valid: BlameSet.t;
  (* Non-empty and disjoint from valid. *)
  invalid: BlameSet.t;
}

(* An empty validation ledger *)
let empty = { valid = BlameSet.empty; invalid = BlameSet.empty }

(* Combine validation ledger information at a join point *)
let join ledger1 ledger2 =
  let old_valid = BlameSet.union ledger1.valid ledger2.valid in
  let old_invalid = BlameSet.union ledger1.invalid ledger2.invalid in
  let valid = BlameSet.inter ledger1.valid ledger2.valid in
  let invalid = BlameSet.union old_invalid (BlameSet.diff old_valid valid) in
  { valid; invalid }

(* Does ledger1 entail ledger2? *)
let sub ledger1 ledger2 =
  BlameSet.subset ledger2.valid ledger1.valid
  && BlameSet.subset ledger1.invalid ledger2.invalid

let is_valid ledger lid = BlameSet.member lid ledger.valid

let is_invalid ledger lid =
  Option.map (BlameSet.find_opt lid ledger.invalid) ~f:snd

let conditionally_forget predicate (ledger : t) blame : t =
  let (to_invalidate, valid) = BlameSet.partition predicate ledger.valid in
  (* Don't allocate if there is nothing to forget *)
  if BlameSet.is_empty to_invalidate then
    ledger
  else
    let to_invalidate = BlameSet.attach_blame blame to_invalidate in
    { valid; invalid = BlameSet.union to_invalidate ledger.invalid }

let forget = conditionally_forget (const true)

let forget_prefixed (ledger : t) prefix_lid blame : t =
  let is_prefixed (fake_id, _blame) =
    String.is_prefix
      ~prefix:(Local_id.to_string prefix_lid ^ "->")
      (Local_id.to_string fake_id)
  in
  conditionally_forget is_prefixed ledger blame

let forget_suffixed (ledger : t) suffix blame : t =
  let is_prefixed (fake_id, _blame) =
    String.is_suffix ~suffix:("->" ^ suffix) (Local_id.to_string fake_id)
  in
  conditionally_forget is_prefixed ledger blame

let forget_static_named (ledger : t) member_name blame : t =
  let is_named (fake_id, _blame) =
    String.is_suffix ~suffix:("::" ^ member_name) (Local_id.to_string fake_id)
  in
  conditionally_forget is_named ledger blame

let add ledger lid pos =
  let reason = Reason.(Blame (pos, BSout_of_scope)) in
  {
    valid = BlameSet.add lid reason ledger.valid;
    invalid = BlameSet.remove lid ledger.invalid;
  }

let blame_as_log_value (Reason.Blame (p, blame_source)) =
  match blame_source with
  | Reason.BScall ->
    Typing_log_value.(make_map [("Blame_call", pos_as_value p)])
  | Reason.BSlambda ->
    Typing_log_value.(make_map [("Blame_lambda", pos_as_value p)])
  | Reason.BSassignment ->
    Typing_log_value.(make_map [("Blame_assigment", pos_as_value p)])
  | Reason.BSout_of_scope ->
    Typing_log_value.(make_map [("Blame_out_of_scope", pos_as_value p)])

let as_log_value ledger =
  let log_blame_set_as_value set =
    Typing_log_value.make_map
      (List.map (BlameSet.elements set) ~f:(fun (lid, blame_opt) ->
           ( Typing_log_value.local_id_as_string lid,
             blame_as_log_value blame_opt )))
  in
  Typing_log_value.(
    make_map
      [
        ( "Fake member ledger",
          make_map
            [
              ("valid", log_blame_set_as_value ledger.valid);
              ("invalid", log_blame_set_as_value ledger.invalid);
            ] );
      ])

let make_id obj_name member_name =
  let obj_name =
    match obj_name with
    | (_, _, Aast.This) -> Typing_defs.this
    | (_, _, Aast.Lvar (_, x)) -> x
    | _ -> assert false
  in
  Local_id.make_unscoped (Local_id.to_string obj_name ^ "->" ^ member_name)

let make_static_id cid member_name =
  let class_name = Nast.class_id_to_str cid in
  Local_id.make_unscoped (class_name ^ "::" ^ member_name)
