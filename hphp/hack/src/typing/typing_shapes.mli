(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val tshape_field_name_with_ty_err :
  Typing_env_types.env ->
  ('a, 'b) Aast.expr ->
  (Typing_defs_core.tshape_field_name, Typing_error.t) result

(** Convert an field as Aast.expr into a tshape_field_name then pass the result to the provided function.
  Handle any conversion error as well. *)
val do_with_field_expr :
  Typing_env_types.env ->
  ('a, 'b) Aast.expr ->
  with_error:'res ->
  (Typing_defs_core.tshape_field_name -> 'res) ->
  'res

(** Refine a shape with the knowledge that field_name
  exists. We do this by intersecting with
  shape(field_name => mixed, ...) *)
val refine_key_exists :
  Typing_defs.TShapeMap.key ->
  Pos.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val refine_not_key_exists :
  Typing_defs.TShapeMap.key ->
  Pos.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

(** [idx_without_default env shape_type field] returns the type of Shapes::idx($s)
  where $s has type [shape_type] *)
val idx_without_default :
  Typing_env_types.env ->
  expr_pos:Pos.t ->
  shape_pos:Pos.t ->
  Typing_defs.locl_ty ->
  Typing_defs.tshape_field_name ->
  Typing_env_types.env * Typing_defs.locl_ty

(** Refine the type of a shape knowing that a call to Shapes::idx is not null.
  This means that the shape now has the field, and that the type for this
  field is not nullable.
  We stay quite liberal here: we add the field to the shape type regardless
  of whether this field can be here at all. Errors will anyway be raised
  elsewhere when typechecking the call to Shapes::idx. This allows for more
  useful typechecking of incomplete code (code in the process of being
  written). *)
val shapes_idx_not_null :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Nast.expr ->
  Typing_env_types.env * Typing_defs.locl_ty

val remove_key :
  Pos.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  ('a, 'b) Aast.expr ->
  Typing_env_types.env * Typing_defs.locl_ty

val to_dict :
  Typing_env_types.env ->
  Pos.t ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_phase Typing_defs.ty ->
  Type_mapper.result

(** Check a class constant annotated with [__ShapeKey], including its initializer
    value, trait restrictions, and shape-key validity. *)
val check_shape_key_const :
  Typing_env_types.env ->
  Pos.t ->
  Nast.class_ ->
  Folded_class.t ->
  Nast.class_const ->
  ('a, 'b) Aast.class_const_kind ->
  Typing_env_types.env

(** Report `self::CONST` or `nameof self` used as a shape key inside a trait. *)
val check_trait_self_shape_key :
  Typing_env_types.env -> ('a, 'b) Aast.expr -> unit

val check_shape_keys_validity :
  Typing_env_types.env ->
  Ast_defs.shape_field_name list ->
  Typing_env_types.env * Typing_error.t list

val transform_special_shapes_fun_ty :
  Typing_defs.tshape_field_name ->
  Aast.sid ->
  int ->
  Typing_defs.decl_fun_type ->
  Typing_defs.decl_fun_type
