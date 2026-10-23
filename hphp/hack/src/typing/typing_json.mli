(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Convert a type to a structured JSON value, as follows:
 * <prim> ::= "int" | "bool" | "float" | "string" | "num" | "arraykey"
 *         | "mixed" | "resource" | "void" | "noreturn" | "null"
 * <array-args> ::= [ <type> ] | [ <type>, <type> ]
 * <type> ::=
 *   mixed
 *     { "kind":"mixed" }
 *   this
 *     { "kind":"this" }
 *   any
 *     { "kind":"any" }
 *   Anonymous lambda types
 *     { "kind":"anon" }
 *   Primitives
 *     { "kind":"primitive", "name":<prim> }
 *   Arrays, with 0, 1 or 2 type arguments
 *     { "kind":"array", "args":<array-args> }
 *   Enums (with optional bound)
 *     { "kind":"enum", "name":<identifier> [,"as":<type>] }
 *   Newtype (with optional bound)
 *     { "kind":"newtype", "name":<identifier>, "args":<type-arguments>
 *        [,"as":<type>] }
 *   Nullable types, with singleton type argument
 *     { "kind":"nullable", "args":<type-arguments> }
 *   Classes (or interfaces, or traits) with 0 or more type arguments
 *     { "kind":"class", "name":<identifier>, "args":<type-arguments> }
 *   Generic parameters
 *     { "kind":"generic", "name":<identifier> }
 *   Tuples (and tuple-like arrays)
 *     { "kind":"tuple", "args":<type-arguments> }
 *   Shapes with fields
 *     { "kind":"shape", "is_array":false, "fields_known":<bool>,
 *       "fields":<fields> [,"unknown_fields_type":<type>] }
 *     When unknown_fields_type is absent, fields_known selects nothing (true)
 *     or mixed (false) as the type of unknown fields. An explicit
 *     unknown_fields_type takes precedence over fields_known.
 *   Shape splats, with operands in merge order
 *     { "kind":"shape_splat", "elems":<type-arguments> }
 *   Function types, with 0 or more arguments and possibly-void result type
 *     { "kind":"function", "args":<type-arguments>, "result":<type> }
 *   Unions of types, with 2 or more arguments
 *     { "kind":"union", "args":<type-arguments> }
 *   Type constant paths, with base type and list of identifiers
 *     { "kind":"path", "type":<type>, "path":<identifiers> [,"as":<type>] }
 *     Base type may be
 *         { "kind":"static" }
 *         { "kind":"this" }
 *         { "kind":"class", "name":<identifier>, "args":[]}
 *       Expression-dependent type
 *         { "kind":"expr" }
 *)
val from_locl_ty :
  Typing_env_types.env ->
  ?show_like_ty:bool ->
  Typing_defs.locl_ty ->
  Yojson.Safe.t

(* Attempt to deserialize a previously-serialized type back into a type we can
   manipulate. Note that this function accesses the global state in
   `Decl_provider` to verify that certain type names exist. *)
val to_locl_ty :
  ?keytrace:Hh_json_helpers.Access.keytrace ->
  Provider_context.t ->
  Yojson.Safe.t ->
  (Typing_defs.locl_ty, Typing_defs.deserialization_error) result
