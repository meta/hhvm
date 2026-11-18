open Ppxlib
open Ast_builder.Default
module IMap = Map.Make (Int)
module SMap = Map.Make (String)
module SSet = Set.Make (String)

module Help = struct
  (** Generate nice type variable names: `'a,'b,...,'z,'a1,...` *)
  let tyvar_name i =
    let (c, n) = (i mod 26, i / 26) in
    let a = String.make 1 @@ char_of_int (c + 97)
    and v =
      if n = 0 then
        ""
      else
        string_of_int n
    in
    a ^ v
end

module Names = struct
  let pass_module_name = "Pass"

  let combine_fn_name = "combine"

  let identity_name = "identity"

  let pass_field_pfx = "on"

  let transform_pfx = "transform"

  let traverse_pfx = "traverse"

  let bottom_up_arg = "bottom_up"

  let top_down_arg = "top_down"

  let ctx_arg = "ctx"

  let stop_variant_label = "Stop"

  let continue_variant_label = "Continue"

  let restart_variant_label = "Restart"

  let opaque_attr = "transform.opaque"

  let explicit_attr = "transform.explicit"
end

module Err = struct
  let raise_unsupported_ty loc =
    Location.raise_errorf
      ~loc
      "The `ppx_transform` preprocessor does not support this type"

  let unsupported_ty loc =
    Location.error_extensionf
      ~loc
      "The `ppx_transform` preprocessor does not support this type"

  let unsupported_open loc =
    Location.error_extensionf
      ~loc
      "Unsupported use of transform (you can only use it on closed types)"

  let unsupported_abstract loc =
    Location.error_extensionf
      ~loc
      "Unsupported use of traverse (you can only use it on non-abstract types)"

  let unsupported_ctor_args loc =
    Location.error_extensionf
      ~loc
      "Explicit transforms of inline records are not supported by the `ppx_transform` preprocessor"

  let unsupported_ctor_args_empty loc =
    Location.error_extensionf
      ~loc
      "Explicit transforms of nullary data constructors are not supported by the `ppx_transform` preprocessor"
end

module Restart = struct
  type encoding =
    | Encode_as_variant
    | Encode_as_result

  type t =
    | Allow
    | Disallow of encoding

  let allowed = function
    | Allow -> true
    | Disallow _ -> false

  let encoding = function
    | Allow -> Encode_as_variant
    | Disallow encoding -> encoding
end

module Maps = struct
  (** Configuration for custom map functions for type constructors.
      Maps type names (e.g., "SMap.t") to the expression for their map function
      (e.g., "SMap.map").
  *)
  type t = string SMap.t

  let empty = SMap.empty

  let of_list lst =
    List.fold_left (fun acc (k, v) -> SMap.add k v acc) SMap.empty lst

  let infer_map_fn type_name =
    match String.rindex_opt type_name '.' with
    | Some i -> String.sub type_name 0 (i + 1) ^ "map"
    | None -> "map_" ^ type_name

  let find_opt = SMap.find_opt

  let add = SMap.add
end

module Annot = struct
  (** Interpreted annotations for the ppx:
      - [Opaque] means we will not transform a type;
      - [Explicit] means we will generate an explicit [transform_...] function
        for something other than a top-level declaration. It can be attached
        to either a record field or a variant constructor.

      It makes no sense to have both on a given element so we represent as a
      sum.
  *)
  type t =
    | Opaque
    | Explicit

  let has_opaque_attr attrs =
    List.exists
      (fun { attr_name = { txt; _ }; _ } -> String.equal Names.opaque_attr txt)
      attrs

  let has_explicit_attr attrs =
    List.exists
      (fun { attr_name = { txt; _ }; _ } ->
        String.equal Names.explicit_attr txt)
      attrs

  let of_attributes attrs =
    let has_explicit = has_explicit_attr attrs
    and has_opaque = has_opaque_attr attrs in
    match (has_explicit, has_opaque) with
    | (true, true) ->
      let loc =
        (List.find
           (fun { attr_name = { txt; _ }; _ } ->
             String.equal txt Names.explicit_attr
             || String.equal txt Names.opaque_attr)
           attrs)
          .attr_loc
      in
      Location.raise_errorf
        ~loc
        "[@transform.opaque] and [@transform.explicit] are mutually exclusive"
    | (true, false) -> Some Explicit
    | (false, true) -> Some Opaque
    | (false, false) -> None
end

module Core_ty = struct
  (** Get the name of a type variable failing when the [core_type] is not a
      type variable  *)
  let tyvar_exn { ptyp_desc; ptyp_loc; _ } =
    match ptyp_desc with
    | Ptyp_var nm -> nm
    | Ptyp_any -> gen_symbol ()
    | _ ->
      Location.raise_errorf
        ~loc:ptyp_loc
        "Expected a type variable in type parameter"

  let ctor_longident_exn { ptyp_desc; ptyp_loc; _ } =
    match ptyp_desc with
    | Ptyp_constr (ident, _) -> ident
    | _ -> Location.raise_errorf ~loc:ptyp_loc "Expected a type constructor"

  (** Replace each named type variable in a [core_type] using the provided
      substitution. This will fail if the substitution does not contain the
      names of all tyvars encountered *)
  let rename_tyvars t ~subst =
    let visitor =
      object
        inherit Ast_traverse.map as super

        method! core_type core_type =
          let core_type = super#core_type core_type in
          match core_type.ptyp_desc with
          | Ptyp_var nm ->
            ptyp_var ~loc:core_type.ptyp_loc
            @@ Option.value ~default:nm
            @@ SMap.find_opt nm subst
          | _ -> core_type
      end
    in
    visitor#core_type t

  (** Replace tyvars with type constructors corresponding to newtypes
      declaration for locally abstract types *)
  let newtypes =
    let visitor =
      object
        inherit Ast_traverse.map as super

        method! core_type core_type =
          let core_type = super#core_type core_type in
          match core_type.ptyp_desc with
          | Ptyp_var nm ->
            let loc = core_type.ptyp_loc in
            ptyp_constr ~loc (Located.lident ~loc nm) []
          | _ -> core_type
      end
    in
    (fun ty -> visitor#core_type ty)

  (** Collect the unique tyvar names with a [core_type] *)
  let tyvars =
    let visitor =
      object
        inherit [label list] Ast_traverse.fold as super

        method! core_type core_type acc =
          let acc = super#core_type core_type acc in
          match core_type.ptyp_desc with
          | Ptyp_var v when not @@ List.exists (String.equal v) acc -> v :: acc
          | _ -> acc
      end
    in
    (fun ?(acc = []) ty -> visitor#core_type ty acc)

  let builtin_prims =
    SSet.of_list
      [
        "int";
        "Int.t";
        "int32";
        "Int32.t";
        "int64";
        "Int64.t";
        "nativeint";
        "Nativeint.t";
        "float";
        "Float.t";
        "bool";
        "Bool.t";
        "char";
        "Char.t";
        "string";
        "String.t";
        "bytes";
        "Bytes.t";
        "unit";
        "Unit.t";
      ]

  let builtin_tycons =
    SSet.of_list
      [
        "ref";
        "result";
        "Result.result";
        "Result.t";
        "option";
        "Option.t";
        "list";
        "List.t";
        "array";
        "Array.t";
        "lazy_t";
        "Lazy.t";
        "Either.t";
        "SMap.t";
        "S_map.t";
        "S_set.t";
        "TShapeMap.t";
        "fun_type";
      ]

  let builtin = SSet.union builtin_prims builtin_tycons

  (** Collect all type constructors referenced in a [core_type] _excluding_
      those in the provided set - note that the excluded ctors are always
      a set of type declarations local to the module we are deriving for  *)
  let tycons =
    let visitor excluded =
      object
        inherit [label list SMap.t] Ast_traverse.fold as super

        method! core_type core_type acc =
          if Annot.has_opaque_attr core_type.ptyp_attributes then
            acc
          else
            match core_type.ptyp_desc with
            | Ptyp_constr ({ txt; loc; _ }, _) ->
              let (tyname, key, path) =
                match List.rev @@ Longident.flatten_exn txt with
                | ty :: path ->
                  ( String.concat "." @@ List.rev (ty :: path),
                    String.concat "" path,
                    path )
                | _ ->
                  Location.raise_errorf ~loc "Invalid type constructor name"
              in
              let is_excluded = SSet.mem tyname excluded
              and is_builtin = SSet.mem tyname builtin in
              (* If the type is excluded it is one of the types we are deriving
                 for so we don't need to recurse. This prevents us from picking
                 up phantom types used in indexing a GADT *)
              let acc =
                if is_excluded || is_builtin then
                  acc
                else
                  SMap.add key path acc
              in
              if is_excluded then
                acc
              else
                super#core_type core_type acc
            | _ -> super#core_type core_type acc
      end
    in
    fun acc ty ~excluded ->
      let visitor = visitor excluded in
      visitor#core_type ty acc
end

module Ident = struct
  (** Each field in our [pass] record type corresponds to either a some element
      of the types for which we are deriving or some externally defined type
      (see [pass_field] for more information).

      For fields corresponding to types for which we are deriving, a field is
      generated for each of:
      -  a type declaration;
      - a constructor of a variant type marked [[@transform.explicit]]; or
      - a field of a record type marked [[@transform.explicit]].

      The [transform_field_ident] type tracks which of these we have and is used
      to generate the name of the field *)
  type t =
    | Type of string
    | Ctor of string * string
    | Field of string * string

  let flatten = function
    | Type ty_name -> [ty_name]
    | Ctor (ty_name, ctor_name) -> [ty_name; ctor_name]
    | Field (ty_name, fld_name) -> [ty_name; fld_name]

  let to_string t = String.concat "_" @@ flatten t

  let kind_string t =
    match t with
    | Type _ -> "ty"
    | Ctor _ -> "ctor"
    | Field _ -> "fld"

  let prefixed pfx t =
    match (t, flatten t) with
    | (Type _, ["t"]) -> [pfx]
    | (_, sfx) ->
      let kind = kind_string t in
      pfx :: kind :: sfx

  let field_name t =
    let sfx = flatten t and kind = kind_string t in
    String.concat "_" (Names.pass_field_pfx :: kind :: sfx)

  let transform_fn_name t = String.concat "_" @@ prefixed Names.transform_pfx t

  let traverse_fn_name t = String.concat "_" @@ prefixed Names.traverse_pfx t
end

module Record_field = struct
  (** Simplified [label_declaration] representation *)
  type t = {
    label: label;
    ty: core_type;
    loc: location;
  }

  let of_lbl_decl
      { pld_name = { txt; _ }; pld_type; pld_loc; pld_attributes; _ } ~subst =
    ( { label = txt; ty = Core_ty.rename_tyvars pld_type ~subst; loc = pld_loc },
      Annot.of_attributes pld_attributes )
end

module Variant_ctor = struct
  (** Simplified [constructor_declaration] representation  *)
  type t =
    | Constant_ctor of string * location
    | Single_ctor of string * location * core_type
    | Tuple_ctor of string * location * core_type list
    | Record_ctor of string * location * (Record_field.t * Annot.t option) list

  let of_ctor_decl
      { pcd_name = { txt; _ }; pcd_args; pcd_loc; pcd_attributes; _ } ~subst =
    let annot_opt = Annot.of_attributes pcd_attributes in
    match pcd_args with
    | Pcstr_tuple [] -> (Constant_ctor (txt, pcd_loc), annot_opt)
    | Pcstr_tuple [ty] ->
      (Single_ctor (txt, pcd_loc, Core_ty.rename_tyvars ty ~subst), annot_opt)
    | Pcstr_tuple tys ->
      ( Tuple_ctor (txt, pcd_loc, List.map (Core_ty.rename_tyvars ~subst) tys),
        annot_opt )
    | Pcstr_record lbl_decls ->
      ( Record_ctor
          (txt, pcd_loc, List.map (Record_field.of_lbl_decl ~subst) lbl_decls),
        annot_opt )
end

module Variant = struct
  type t = {
    name: string;
    loc: location;
    tyvars: label list;
    ctors: (Variant_ctor.t * Annot.t option) list;
    opaque: bool;
  }
end

module Gadt = struct
  type t = {
    name: string;
    loc: location;
    tyvars: label list;
    ctors: ((Variant_ctor.t * Annot.t option) * core_type) list;
    opaque: bool;
  }
end

module Record = struct
  type t = {
    name: string;
    loc: location;
    tyvars: label list;
    fields: (Record_field.t * Annot.t option) list;
    opaque: bool;
  }
end

module Alias = struct
  type t = {
    name: string;
    loc: location;
    tyvars: label list;
    ty: core_type;
    opaque: bool;
  }
end

module Unsupported = struct
  type kind =
    | Abstract
    | Open

  type t = {
    loc: location;
    ident: Ident.t;
    kind: kind;
  }
end

module Decl = struct
  (** Simplified [type_declaration] representation *)
  type t =
    | Variant of Variant.t
    | Gadt of Gadt.t
    | Record of Record.t
    | Alias of Alias.t
    | Unsupported of Unsupported.t

  (** Check if _any_ data constructor has a result type to determine if the
      declared type should be treated as a GADT  *)
  let is_gadt ctors =
    List.exists
      (function
        | { pcd_res = Some _; _ } -> true
        | _ -> false)
      ctors

  let of_ty_decl
      {
        ptype_name = { txt = name; _ };
        ptype_loc = loc;
        ptype_params;
        ptype_kind;
        ptype_manifest;
        ptype_attributes;
        _;
      } =
    let opaque = Annot.has_opaque_attr ptype_attributes
    and (subst, tyvars) =
      let (subst, tyvars) =
        List.split
        @@ List.mapi
             (fun i (ty, _) ->
               let new_nm = Help.tyvar_name i in
               let old_nm = Core_ty.tyvar_exn ty in
               ((old_nm, new_nm), new_nm))
             ptype_params
      in
      let subst =
        List.fold_left (fun acc (k, v) -> SMap.add k v acc) SMap.empty subst
      in
      (subst, tyvars)
    in
    match ptype_kind with
    | Ptype_variant ctor_decls when is_gadt ctor_decls ->
      let ctors =
        List.map
          (fun ctor_decl ->
            (* A GADT may contain regular ADT data constructor declarations; in this case, we need
               to construct a type based on the [ptype_name] and [ptype_params] *)
            let ty =
              match ctor_decl.pcd_res with
              | Some ty -> ty
              | None ->
                ptyp_constr ~loc (Located.lident ~loc name)
                @@ List.map (ptyp_var ~loc) tyvars
            in

            (Variant_ctor.of_ctor_decl ctor_decl ~subst, ty))
          ctor_decls
      in
      Gadt Gadt.{ name; loc; tyvars; ctors; opaque }
    | Ptype_variant ctor_decls ->
      let ctors = List.map (Variant_ctor.of_ctor_decl ~subst) ctor_decls in
      Variant Variant.{ name; loc; tyvars; ctors; opaque }
    | Ptype_record lbl_decls ->
      let fields = List.map (Record_field.of_lbl_decl ~subst) lbl_decls in
      Record Record.{ name; loc; tyvars; fields; opaque }
    | Ptype_abstract ->
      (match ptype_manifest with
      | Some ty ->
        let ty = Core_ty.rename_tyvars ty ~subst in
        Alias Alias.{ name; loc; tyvars; ty; opaque }
      | None -> Variant Variant.{ name; loc; tyvars; ctors = []; opaque })
    | Ptype_open ->
      Unsupported Unsupported.{ ident = Ident.Type name; loc; kind = Open }

  let opaque_info t =
    match t with
    | Gadt Gadt.{ name; opaque; _ }
    | Variant Variant.{ name; opaque; _ }
    | Record Record.{ name; opaque; _ }
    | Alias Alias.{ name; opaque; _ } ->
      (name, opaque)
    | Unsupported Unsupported.{ ident; _ } ->
      (* Use [to_string] since this is always a type *)
      (Ident.to_string ident, false)

  let name t =
    match t with
    | Gadt Gadt.{ name; _ }
    | Variant Variant.{ name; _ }
    | Record Record.{ name; _ }
    | Alias Alias.{ name; _ } ->
      name
    | Unsupported Unsupported.{ ident; _ } -> Ident.to_string ident

  (* OCaml requires explicit universal quantifiers on `let rec` bindings in
     two cases: (1) polymorphic recursion — a recursive occurrence uses
     different type arguments than the declaration, so the monomorphic
     recursion variable that OCaml infers by default would be wrong; and
     (2) GADTs — pattern matching introduces rigid type variables that
     can only unify with a polymorphically-typed callee. *)
  let is_nonregular_in_core_ty tyname tyvars ty =
    let rec aux { ptyp_desc; _ } =
      match ptyp_desc with
      | Ptyp_constr ({ txt; _ }, tys) ->
        (match Longident.flatten_exn txt with
        | [nm] when String.equal tyname nm -> auxs tys tyvars
        | _ -> false)
      | Ptyp_any
      | Ptyp_var _ ->
        false
      | Ptyp_alias (ty, _) -> aux ty
      | Ptyp_arrow (_, t1, t2) -> aux t1 || aux t2
      | Ptyp_tuple tys
      | Ptyp_class (_, tys) ->
        List.exists aux tys
      | Ptyp_object (flds, _) -> List.exists aux_obj_fld flds
      | Ptyp_variant (row_flds, _, _) -> List.exists aux_row_fld row_flds
      | Ptyp_poly (_, ty) -> aux ty
      | _ -> Err.raise_unsupported_ty ty.ptyp_loc
    and aux_obj_fld fld =
      match fld.pof_desc with
      | Otag (_, ty)
      | Oinherit ty ->
        aux ty
    and aux_row_fld fld =
      match fld.prf_desc with
      | Rtag (_, _, tys) -> List.exists aux tys
      | Rinherit ty -> aux ty
    and auxs tys tyvars =
      match (tys, tyvars) with
      | ([], []) -> false
      | ({ ptyp_desc; _ } :: tys, tv :: tyvars) ->
        (match ptyp_desc with
        | Ptyp_var nm when String.equal nm tv -> auxs tys tyvars
        | _ -> true)
      | _ ->
        (* This should not happen with valid OCaml types *)
        false
    in
    aux ty

  let is_nonregular_in_fld tyname tyvars Record_field.{ ty; _ } =
    is_nonregular_in_core_ty tyname tyvars ty

  let is_nonregular_in_ctor tyname tyvars ctor =
    match ctor with
    | Variant_ctor.Constant_ctor _ -> false
    | Variant_ctor.Single_ctor (_, _, ty) ->
      is_nonregular_in_core_ty tyname tyvars ty
    | Variant_ctor.Tuple_ctor (_, _, tys) ->
      List.exists (is_nonregular_in_core_ty tyname tyvars) tys
    | Variant_ctor.Record_ctor (_, _, flds) ->
      List.exists (fun (fld, _) -> is_nonregular_in_fld tyname tyvars fld) flds

  let is_nonregular_in tyname tyvars decl =
    match decl with
    | Gadt Gadt.{ ctors; _ } ->
      List.exists
        (fun ((ctor, _), ret_ty) ->
          is_nonregular_in_ctor tyname tyvars ctor
          || is_nonregular_in_core_ty tyname tyvars ret_ty)
        ctors
    | Variant Variant.{ ctors; _ } ->
      List.exists
        (fun (ctor, _) -> is_nonregular_in_ctor tyname tyvars ctor)
        ctors
    | Record Record.{ fields; _ } ->
      List.exists
        (fun (fld, _) -> is_nonregular_in_fld tyname tyvars fld)
        fields
    | Alias Alias.{ ty; _ } -> is_nonregular_in_core_ty tyname tyvars ty
    | Unsupported _ -> false

  let is_regular tyname tyvars decls =
    not @@ List.exists (is_nonregular_in tyname tyvars) decls
end

module Graph : sig
  (** Generate a topologically sorted list of [Decl.t]s grouped into their
      strongly connected components. Why? We want to analyse whether we
      have a non-regular datatype so we can later generate explicit
      universal quantifiers and this requires us to looks for non-regular
      occurrences on the right hand side of any declaration within the
      strongly connected components. This has the pleasant side-effect of
      generating value bindings without redundant `let rec ... and ..`s *)
  val stratify : Decl.t list -> Decl.t list list
end = struct
  let depth_first g =
    let len = Array.length g in
    let marked = Array.make len false and stack = Array.make len ~-1 in
    let idx = ref 0 in
    let push v =
      stack.(!idx) <- v;
      incr idx
    in
    let rec aux v =
      if not marked.(v) then (
        marked.(v) <- true;
        List.iter aux g.(v);
        push v
      )
    in
    for v = 0 to len - 1 do
      aux v
    done;
    stack

  let transpose g =
    let len = Array.length g in
    let g' = Array.make len [] in
    let edge src dst = g'.(src) <- dst :: g'.(src) in
    Array.iteri (fun src dsts -> List.iter (fun dst -> edge dst src) dsts) g;
    g'

  let mark g order =
    let len = Array.length g and g = transpose g in
    let marked = Array.make len false and id = Array.make len ~-1 in
    let count = ref 0 in
    let rec aux v =
      if not marked.(v) then (
        marked.(v) <- true;
        id.(v) <- !count;
        List.iter aux g.(v)
      )
    in
    for i = len - 1 downto 0 do
      let v = order.(i) in
      if not marked.(v) then (
        aux v;
        incr count
      )
    done;
    (id, !count)

  let scc g = mark g @@ depth_first g

  let edges_core_ty fwd ty =
    let rec aux acc ty =
      match ty.ptyp_desc with
      | Ptyp_any
      | Ptyp_var _ ->
        acc
      | Ptyp_alias (ty, _) -> aux acc ty
      | Ptyp_arrow (_, ty1, ty2) -> aux (aux acc ty1) ty2
      | Ptyp_tuple tys
      | Ptyp_class (_, tys) ->
        List.fold_left aux acc tys
      | Ptyp_object (flds, _) -> List.fold_left aux_obj_fld acc flds
      | Ptyp_variant (flds, _, _) -> List.fold_left aux_row_fld acc flds
      | Ptyp_constr (ident, tys) ->
        let idx_opt =
          SMap.find_opt
            (String.concat "." @@ Longident.flatten_exn ident.txt)
            fwd
        in
        let acc =
          Option.value ~default:acc
          @@ Option.map (fun idx -> idx :: acc) idx_opt
        in
        List.fold_left aux acc tys
      | Ptyp_poly (_, ty) -> aux acc ty
      | _ -> Err.raise_unsupported_ty ty.ptyp_loc
    and aux_obj_fld acc fld =
      match fld.pof_desc with
      | Oinherit ty
      | Otag (_, ty) ->
        aux acc ty
    and aux_row_fld acc fld =
      match fld.prf_desc with
      | Rtag (_, _, tys) -> List.fold_left aux acc tys
      | Rinherit ty -> aux acc ty
    in
    aux [] ty

  let edges_lbl_decl fwd Record_field.{ ty; _ } = edges_core_ty fwd ty

  let edges_ctor_decl fwd ctor =
    match ctor with
    | Variant_ctor.Constant_ctor _ -> []
    | Variant_ctor.Single_ctor (_, _, ty) -> edges_core_ty fwd ty
    | Variant_ctor.Tuple_ctor (_, _, tys) ->
      List.concat_map (edges_core_ty fwd) tys
    | Variant_ctor.Record_ctor (_, _, flds) ->
      List.concat_map (fun (fld, _) -> edges_lbl_decl fwd fld) flds

  let edges fwd decl =
    match decl with
    | Decl.Alias Alias.{ ty; _ } -> edges_core_ty fwd ty
    | Decl.Variant Variant.{ ctors; _ } ->
      List.concat_map (fun (ctor, _) -> edges_ctor_decl fwd ctor) ctors
    | Decl.Gadt Gadt.{ ctors; _ } ->
      List.concat_map (fun ((ctor, _), _) -> edges_ctor_decl fwd ctor) ctors
    | Decl.Record Record.{ fields; _ } ->
      List.concat_map (fun (fld, _) -> edges_lbl_decl fwd fld) fields
    | _ -> []

  let stratify decls =
    let (fwd, bwd) =
      let ls = List.mapi (fun i decl -> (Decl.name decl, i, decl)) decls in
      List.fold_left
        (fun (sm, im) (nm, i, ty_decl) ->
          (SMap.add nm i sm, IMap.add i ty_decl im))
        (SMap.empty, IMap.empty)
        ls
    in
    let adj = Array.of_list @@ List.map (edges fwd) decls in
    let (strata, nstrata) = scc adj in
    let out = Array.make nstrata [] in
    Array.iteri
      (fun idx stratum -> out.(stratum) <- IMap.find idx bwd :: out.(stratum))
      strata;
    List.rev @@ Array.to_list out
end

module Analyse = struct
  type t = {
    decls: Decl.t list;
    opaque_map: bool SMap.t;
    decl_names: SSet.t;
    maps: Maps.t;
  }

  let analyse tds ~maps =
    let decls = List.map Decl.of_ty_decl tds in
    let (opaque_map, decl_names) =
      List.fold_left
        (fun (mp, st) decl ->
          let (name, opaque) = Decl.opaque_info decl in
          (SMap.add name opaque mp, SSet.add name st))
        (SMap.empty, SSet.empty)
        decls
    in
    { decls; opaque_map; decl_names; maps }
end

module Transform_field : sig
  type definition =
    | Variant_ctors of string * (Variant_ctor.t * Annot.t option) list
    | Record_fields of string * (Record_field.t * Annot.t option) list
    | Core_ty of core_type

  type type_info =
    | Regular of bool
    | Locally_abstract

  type def = {
    ident: Ident.t;
    loc: location;
    ty: core_type;
    tyvars: label list;
    definition: definition;
    type_info: type_info;
  }

  type t =
    | Field of def
    | Unsupported of Unsupported.t

  val fields : Analyse.t -> t list list
end = struct
  type definition =
    | Variant_ctors of string * (Variant_ctor.t * Annot.t option) list
    | Record_fields of string * (Record_field.t * Annot.t option) list
    | Core_ty of core_type

  type type_info =
    | Regular of bool
    | Locally_abstract

  type def = {
    ident: Ident.t;
    loc: location;
    ty: core_type;
    tyvars: label list;
    definition: definition;
    type_info: type_info;
  }

  type t =
    | Field of def
    | Unsupported of Unsupported.t

  (** Try to create a [Transform_field.t] from a [variant_ctor]. If the
      variant constructor has an inline record argument we fail since the
      type doesn't exist at the top level so we define a function accepting such
      a type *)
  let variant_ctor_field_opt ctor ~type_name =
    let open Variant_ctor in
    match ctor with
    | Record_ctor _
    | Constant_ctor _ ->
      None
    | Single_ctor (ctor_name, loc, ty) ->
      let tyvars = List.rev @@ Core_ty.tyvars ty in
      let ident = Ident.Ctor (type_name, ctor_name) in
      Some
        (Field
           {
             ident;
             tyvars;
             ty;
             loc;
             type_info = Regular true;
             definition = Core_ty ty;
           })
    | Tuple_ctor (ctor_name, loc, tys) ->
      let ident = Ident.Ctor (type_name, ctor_name) in
      let tyvars =
        List.rev @@ List.fold_left (fun acc ty -> Core_ty.tyvars ~acc ty) [] tys
      in
      let ty = ptyp_tuple ~loc tys in
      Some
        (Field
           {
             ident;
             tyvars;
             ty;
             loc;
             type_info = Regular true;
             definition = Core_ty ty;
           })

  (** Create a list of [Transform_field.t]s for a variant type declaration.

      If the entire variant is marked [[@transform.opaque]] this will be the
      empty list.

      Otherwise, we will generate fields corresponding to:
      - the top-level variant type
      - each constructor marked as [[@transform.explicit]]
  *)
  let variant_fields Variant.{ name; loc; tyvars; ctors; opaque } ~decls =
    if opaque then
      []
    else
      let ident = Ident.Type name in
      let ty =
        ptyp_constr ~loc (Located.lident ~loc name)
        @@ List.map (ptyp_var ~loc) tyvars
      in
      let ty_field =
        Field
          {
            ident;
            tyvars;
            ty;
            loc;
            definition = Variant_ctors (name, ctors);
            type_info = Regular (Decl.is_regular name tyvars decls);
          }
      in
      let ctor_fields =
        List.filter_map
          (fun (ctor, annot_opt) ->
            Option.bind annot_opt (function
                | Annot.Opaque -> None
                | Annot.Explicit -> variant_ctor_field_opt ctor ~type_name:name))
          ctors
      in
      ty_field :: ctor_fields

  let gadt_fields Gadt.{ name; loc; tyvars; ctors; opaque } =
    if opaque then
      []
    else
      let ident = Ident.Type name in
      let ty =
        ptyp_constr ~loc (Located.lident ~loc name)
        @@ List.map (ptyp_var ~loc) tyvars
      in
      let ty_field =
        Field
          {
            ident;
            tyvars;
            ty;
            loc;
            type_info = Locally_abstract;
            definition = Variant_ctors (name, List.map fst ctors);
          }
      in
      let ctor_fields =
        List.filter_map
          (fun ((ctor, annot_opt), _) ->
            Option.bind annot_opt (function
                | Annot.Opaque -> None
                | Annot.Explicit -> variant_ctor_field_opt ctor ~type_name:name))
          ctors
      in
      ty_field :: ctor_fields

  let record_field_field Record_field.{ label; ty; loc } ~type_name =
    let ident = Ident.Field (type_name, label) in
    let tyvars = List.rev @@ Core_ty.tyvars ty in
    Field
      {
        ident;
        tyvars;
        ty;
        loc;
        type_info = Regular true;
        definition = Core_ty ty;
      }

  (** Create a list of  [Transform_field.t]s for a record type declaration.

      If the entire record is marked [[@transform.opaque]] this will be the
      empty list.

      Otherwise, we will generate fields corresponding to:
      - the top-level record type
      - each field marked as [[@transform.explicit]]
  *)
  let record_fields Record.{ name; loc; tyvars; fields; opaque } ~decls =
    if opaque then
      []
    else
      let ident = Ident.Type name in
      let ty =
        ptyp_constr ~loc (Located.lident ~loc name)
        @@ List.map (ptyp_var ~loc) tyvars
      in
      let ty_field =
        Field
          {
            ident;
            tyvars;
            ty;
            loc;
            definition = Record_fields (name, fields);
            type_info = Regular (Decl.is_regular name tyvars decls);
          }
      in
      let field_fields =
        List.filter_map
          (fun (fld, annot_opt) ->
            Option.bind annot_opt (function
                | Annot.Opaque -> None
                | Annot.Explicit ->
                  Some (record_field_field fld ~type_name:name)))
          fields
      in
      ty_field :: field_fields

  (** Try to generate a  [Transform_field.t] for an alias type declaration.
      This will be [None] if the alias is marked [[@transform.opaque]]  *)
  let alias_transform_field_opt
      Alias.{ name; tyvars; ty; opaque; loc; _ } ~decls =
    if opaque then
      None
    else
      let ident = Ident.Type name in
      Some
        (Field
           {
             ident;
             tyvars;
             ty =
               ptyp_constr ~loc (Located.lident ~loc name)
               @@ List.map (ptyp_var ~loc) tyvars;
             loc;
             definition = Core_ty ty;
             type_info = Regular (Decl.is_regular name tyvars decls);
           })

  let decl_fields decl ~decls =
    match decl with
    | Decl.Variant variant -> variant_fields variant ~decls
    | Decl.Gadt gadt -> gadt_fields gadt
    | Decl.Record record -> record_fields record ~decls
    | Decl.Alias alias ->
      Option.to_list @@ alias_transform_field_opt alias ~decls
    | Decl.Unsupported unsupported -> [Unsupported unsupported]

  (* OCaml cannot generalize polymorphic recursion in a `let rec ... and ...`
     group unless every binding has an explicit quantifier. We prefer to
     omit them for regular types: explicit quantifiers force OCaml to
     instantiate the type scheme at every recursive call, whereas
     monomorphic recursion variables are unified once for the whole group. *)
  let needs_explicit_quantifiers flds =
    List.exists
      (function
        | Field { type_info = Locally_abstract; _ }
        | Field { type_info = Regular false; _ } ->
          true
        | _ -> false)
      flds

  let force_quantifiers flds =
    if needs_explicit_quantifiers flds then
      List.map
        (function
          | Field ({ type_info = Regular true; _ } as def) ->
            Field { def with type_info = Regular false }
          | fld -> fld)
        flds
    else
      flds

  let fields Analyse.{ decls; _ } =
    List.filter_map (fun decls ->
        match List.concat_map (decl_fields ~decls) decls with
        | [] -> None
        | xs -> Some (force_quantifiers xs))
    @@ Graph.stratify decls
end

module Pass_field : sig
  val fields : Analyse.t -> label list list
end = struct
  let record_field_fields acc (Record_field.{ ty; _ }, annot_opt) ~excluded =
    match annot_opt with
    | Some Annot.Opaque -> acc
    | _ -> Core_ty.tycons acc ty ~excluded

  let variant_ctor_fields acc (variant_ctor, annot_opt) ~excluded =
    match annot_opt with
    | Some Annot.Opaque -> acc
    | _ ->
      let open Variant_ctor in
      (match variant_ctor with
      | Constant_ctor _ -> acc
      | Single_ctor (_, _, ty) -> Core_ty.tycons acc ty ~excluded
      | Tuple_ctor (_, _, tys) ->
        List.fold_left (Core_ty.tycons ~excluded) acc tys
      | Record_ctor (_, _, flds) ->
        List.fold_left (record_field_fields ~excluded) acc flds)

  let decl_fields acc decl ~excluded =
    let open Decl in
    match decl with
    | Variant Variant.{ ctors; opaque; _ } ->
      if opaque then
        acc
      else
        List.fold_left (variant_ctor_fields ~excluded) acc ctors
    | Gadt Gadt.{ ctors; opaque; _ } ->
      if opaque then
        acc
      else
        List.fold_left
          (fun acc (ctor, _) -> variant_ctor_fields acc ctor ~excluded)
          acc
          ctors
    | Record Record.{ fields; opaque; _ } ->
      if opaque then
        acc
      else
        List.fold_left (record_field_fields ~excluded) acc fields
    | Alias Alias.{ ty; opaque; _ } ->
      if opaque then
        acc
      else
        Core_ty.tycons acc ty ~excluded
    | _ -> acc

  let fields Analyse.{ decls; decl_names = excluded; _ } =
    List.map snd
    @@ SMap.bindings
    @@ List.fold_left (decl_fields ~excluded) SMap.empty decls
end

module Gen_pass : sig
  val gen_sig :
    Transform_field.t list ->
    label list list ->
    loc:location ->
    allow_restart:Restart.t ->
    signature_item

  val gen_str :
    Transform_field.t list ->
    label list list ->
    loc:location ->
    allow_restart:Restart.t ->
    structure_item
end = struct
  let mk_tag txt ~ty ~loc = rtag ~loc (Located.mk ~loc txt) false [ty]

  let gen_continuation_ty ty ~loc ~allow_restart =
    match allow_restart with
    | Restart.Allow ->
      let tags =
        List.map
          (mk_tag ~loc ~ty)
          Names.
            [stop_variant_label; continue_variant_label; restart_variant_label]
      in
      ptyp_variant ~loc tags Closed None
    | Restart.(Disallow Encode_as_variant) ->
      let tags =
        List.map
          (mk_tag ~loc ~ty)
          Names.[stop_variant_label; continue_variant_label]
      in
      ptyp_variant ~loc tags Closed None
    | Restart.(Disallow Encode_as_result) ->
      ptyp_constr ~loc (Located.lident ~loc "result") [ty; ty]

  let gen_transform_field_def
      Transform_field.{ ident; tyvars; ty; _ } ~loc ~allow_restart =
    let ty =
      match ident with
      | Ident.Type name ->
        ptyp_constr ~loc (Located.lident ~loc name)
        @@ List.map (ptyp_var ~loc) tyvars
      | _ -> { ty with ptyp_attributes = [] }
    in
    let cont_ty = gen_continuation_ty ty ~loc ~allow_restart in
    let ctx = ptyp_var ~loc Names.ctx_arg in
    let transform_ty =
      [%type: ([%t ty] -> ctx:[%t ctx] -> [%t ctx] * [%t cont_ty]) option]
    in
    let type_ =
      ptyp_poly ~loc (List.map (Located.mk ~loc) tyvars) transform_ty
    in
    label_declaration
      ~loc
      ~name:(Located.mk ~loc (Ident.field_name ident))
      ~mutable_:Immutable
      ~type_

  let gen_transform_field fld ~loc ~allow_restart =
    match fld with
    | Transform_field.Field def ->
      gen_transform_field_def def ~loc ~allow_restart
    | Transform_field.Unsupported Unsupported.{ ident; _ } ->
      let type_ = ptyp_extension ~loc @@ Err.unsupported_ctor_args loc in
      label_declaration
        ~loc
        ~name:(Located.mk ~loc (Ident.field_name ident))
        ~mutable_:Immutable
        ~type_

  let gen_pass_field path ~loc =
    (* Given some identifier for a type, we expect the corresponding [pass] ty
       to be located in a module named [Transform] *)
    let (txt, field_name) =
      ( Longident.parse
        @@ String.concat "."
        @@ List.rev ("t" :: Names.pass_module_name :: path),
        String.concat "_" (Names.pass_field_pfx :: List.rev path) )
    in
    let pass_ty =
      ptyp_constr ~loc (Located.mk ~loc txt) [ptyp_var ~loc Names.ctx_arg]
    in
    label_declaration
      ~loc
      ~name:(Located.mk ~loc field_name)
      ~mutable_:Immutable
      ~type_:[%type: [%t pass_ty] option]

  let params loc = [(ptyp_var ~loc Names.ctx_arg, (NoVariance, NoInjectivity))]

  let gen_kind transform_flds pass_flds ~loc ~allow_restart =
    Ptype_record
      (List.map (gen_transform_field ~loc ~allow_restart) transform_flds
      @ List.map (gen_pass_field ~loc) pass_flds)

  let gen_sig transform_flds pass_flds ~loc ~allow_restart =
    let params = params loc
    and kind = gen_kind transform_flds pass_flds ~loc ~allow_restart in
    psig_type
      ~loc
      Nonrecursive
      [
        type_declaration
          ~loc
          ~name:(Located.mk ~loc "t")
          ~params
          ~cstrs:[]
          ~private_:Public
          ~manifest:None
          ~kind;
      ]

  let gen_str transform_flds pass_flds ~loc ~allow_restart =
    let params = params loc
    and kind = gen_kind transform_flds pass_flds ~loc ~allow_restart in
    pstr_type
      ~loc
      Nonrecursive
      [
        type_declaration
          ~loc
          ~name:(Located.mk ~loc "t")
          ~params
          ~cstrs:[]
          ~private_:Public
          ~manifest:None
          ~kind;
      ]
end

module Gen_identity : sig
  val gen_sig : location -> signature_item

  val gen_str :
    Transform_field.t list -> label list list -> loc:location -> structure_item
end = struct
  let gen_transform_field_def Transform_field.{ ident; loc; _ } =
    let fld_name = Ident.field_name ident in
    (Located.lident ~loc fld_name, [%expr None])

  let gen_transform_field fld =
    match fld with
    | Transform_field.Field def -> gen_transform_field_def def
    | Transform_field.Unsupported Unsupported.{ loc; ident; kind } ->
      let x =
        match kind with
        | Unsupported.Abstract -> Err.unsupported_abstract loc
        | Unsupported.Open -> Err.unsupported_open loc
      in
      (Located.lident ~loc (Ident.field_name ident), pexp_extension ~loc x)

  let gen_pass_field path ~loc =
    let txt =
      lident @@ String.concat "_" (Names.pass_field_pfx :: List.rev path)
    in
    (Located.mk ~loc txt, [%expr None])

  let gen_str transform_fields pass_fields ~loc =
    let transform_exprs = List.map gen_transform_field transform_fields
    and pass_exprs = List.map (gen_pass_field ~loc) pass_fields in
    let pat = ppat_var ~loc (Located.mk ~loc Names.identity_name) in
    let body_expr = pexp_record ~loc (transform_exprs @ pass_exprs) None in
    let expr = [%expr (fun _ -> [%e body_expr])] in
    pstr_value ~loc Nonrecursive [value_binding ~loc ~pat ~expr]

  let gen_sig loc =
    let name = Located.mk ~loc Names.identity_name in
    let cstr_type =
      ptyp_constr ~loc (Located.lident ~loc "t") [ptyp_var ~loc Names.ctx_arg]
    in
    let type_ = [%type: unit -> [%t cstr_type]] in
    psig_value ~loc @@ value_description ~loc ~name ~type_ ~prim:[]
end

module Gen_combine = struct
  let gen_transform_field_def
      elem1 elem2 Transform_field.{ ident; loc; _ } ~allow_restart =
    let fld_name = Ident.field_name ident in
    let fld_lident = Located.lident ~loc fld_name in
    let elem_expr = pexp_ident ~loc (Located.lident ~loc "elem") in
    let elem_pat = ppat_var ~loc (Located.mk ~loc "elem") in
    let ctx = pexp_ident ~loc (Located.lident ~loc Names.ctx_arg) in
    let t1 = pexp_ident ~loc (Located.lident ~loc "t1")
    and t2 = pexp_ident ~loc (Located.lident ~loc "t2") in
    let ident1 = pexp_ident ~loc (Located.lident ~loc elem1)
    and ident2 = pexp_ident ~loc (Located.lident ~loc elem2) in
    let proj1 = pexp_field ~loc ident1 fld_lident
    and proj2 = pexp_field ~loc ident2 fld_lident in
    let app1 =
      pexp_apply ~loc t1 [(Nolabel, elem_expr); (Labelled Names.ctx_arg, ctx)]
    in
    let app2 =
      pexp_apply ~loc t2 [(Nolabel, elem_expr); (Labelled Names.ctx_arg, ctx)]
    in
    let constr =
      match Restart.encoding allow_restart with
      | Restart.Encode_as_variant ->
        ppat_variant ~loc Names.continue_variant_label (Some elem_pat)
      | Restart.Encode_as_result ->
        ppat_construct ~loc (Located.lident ~loc "Ok") (Some elem_pat)
    in
    let match_expr =
      [%expr
        match [%e app1] with
        | (ctx, [%p constr]) -> [%e app2]
        | otherwise -> otherwise]
    in
    let fn_expr =
      pexp_fun ~loc Nolabel None (ppat_var ~loc (Located.mk ~loc "elem"))
      @@ pexp_fun
           ~loc
           (Labelled Names.ctx_arg)
           None
           (ppat_var ~loc (Located.mk ~loc Names.ctx_arg))
           match_expr
    in
    let expr =
      [%expr
        match ([%e proj1], [%e proj2]) with
        | (Some t1, Some t2) -> Some [%e fn_expr]
        | (None, _) -> [%e proj2]
        | _ -> [%e proj1]]
    in
    (Located.lident ~loc fld_name, expr)

  let gen_transform_field elem1 elem2 fld ~allow_restart =
    match fld with
    | Transform_field.Field def ->
      gen_transform_field_def elem1 elem2 def ~allow_restart
    | Transform_field.Unsupported Unsupported.{ ident; loc; kind } ->
      let fld_name = Ident.field_name ident in
      let ext =
        match kind with
        | Unsupported.Abstract -> Err.unsupported_abstract loc
        | Unsupported.Open -> Err.unsupported_open loc
      in
      let expr = pexp_extension ~loc ext in
      (Located.lident ~loc fld_name, expr)

  let gen_pass_field elem1 elem2 loc path =
    let (fn_name, fld_name) =
      ( Longident.parse
        @@ String.concat "."
        @@ List.rev (Names.combine_fn_name :: Names.pass_module_name :: path),
        String.concat "_" (Names.pass_field_pfx :: List.rev path) )
    in
    let fn_expr = pexp_ident ~loc (Located.mk ~loc fn_name) in
    let fld_lident = Located.lident ~loc fld_name in
    let ident1 = pexp_ident ~loc (Located.lident ~loc elem1)
    and ident2 = pexp_ident ~loc (Located.lident ~loc elem2) in
    let proj1 = pexp_field ~loc ident1 fld_lident
    and proj2 = pexp_field ~loc ident2 fld_lident in
    let expr =
      [%expr
        match ([%e proj1], [%e proj2]) with
        | (Some p1, Some p2) -> Some ([%e fn_expr] p1 p2)
        | (Some _, _) -> [%e proj1]
        | _ -> [%e proj2]]
    in
    (Located.lident ~loc fld_name, expr)

  let gen_str transform_fields pass_fields ~loc ~allow_restart =
    let elem1 = "p1" and elem2 = "p2" in
    let transform_exprs =
      List.map (gen_transform_field elem1 elem2 ~allow_restart) transform_fields
    and pass_exprs = List.map (gen_pass_field elem1 elem2 loc) pass_fields in
    let pat = ppat_var ~loc (Located.mk ~loc Names.combine_fn_name)
    and body_expr = pexp_record ~loc (transform_exprs @ pass_exprs) None in
    let pat1 = ppat_var ~loc (Located.mk ~loc elem1)
    and pat2 = ppat_var ~loc (Located.mk ~loc elem2) in
    let expr = [%expr (fun [%p pat1] [%p pat2] -> [%e body_expr])] in
    pstr_value ~loc Nonrecursive [value_binding ~loc ~pat ~expr]

  let gen_sig loc =
    let name = Located.mk ~loc Names.combine_fn_name in
    let pass_ty =
      ptyp_constr ~loc (Located.lident ~loc "t") [ptyp_var ~loc Names.ctx_arg]
    in
    let type_ = [%type: [%t pass_ty] -> [%t pass_ty] -> [%t pass_ty]] in
    psig_value ~loc @@ value_description ~loc ~name ~type_ ~prim:[]
end

module Gen_fn = struct
  let gen_fun_ty ty ~loc =
    let pass_ty =
      ptyp_constr
        ~loc
        {
          loc;
          txt =
            Longident.parse @@ String.concat "." [Names.pass_module_name; "t"];
        }
        [ptyp_var ~loc Names.ctx_arg]
    in
    let ctx = ptyp_var ~loc Names.ctx_arg in
    [%type:
      [%t ty] ->
      ctx:[%t ctx] ->
      top_down:[%t pass_ty] ->
      bottom_up:[%t pass_ty] ->
      [%t ty]]

  (** Generate a value binding for a [transform_...] or [traverse_...] function
      given the name, type info, the pattern for the transformed/traverse
      element, and the body expression.
  *)
  let gen_str fn_name ty tyvars type_info elem_pat body_expr loc =
    let fn_pat = ppat_var ~loc (Located.mk ~loc fn_name) in
    let (pat, expr) =
      match type_info with
      | Transform_field.Regular regular ->
        let expr =
          [%expr
            (fun [%p elem_pat] ~ctx ~top_down ~bottom_up -> [%e body_expr])]
        and pat =
          if regular then
            fn_pat
          else
            ppat_constraint
              ~loc
              fn_pat
              (ptyp_poly ~loc (List.map (Located.mk ~loc) tyvars)
              @@ gen_fun_ty ty ~loc)
        in
        (pat, expr)
      | Transform_field.Locally_abstract ->
        let expr =
          let ty = Core_ty.newtypes ty in
          let expr =
            [%expr
              fun ([%p elem_pat] : [%t ty]) ~ctx ~top_down ~bottom_up : [%t ty] ->
                [%e body_expr]]
          in
          List.fold_right
            (fun tyvar expr -> pexp_newtype ~loc (Located.mk ~loc tyvar) expr)
            tyvars
            expr
        and pat =
          ppat_constraint
            ~loc
            fn_pat
            (ptyp_poly ~loc (List.map (Located.mk ~loc) tyvars)
            @@ gen_fun_ty ty ~loc)
        in
        (pat, expr)
    in
    value_binding ~loc ~pat ~expr
end

module Gen_transform = struct
  let gen_str_def
      Transform_field.{ ident; ty; tyvars; loc; type_info; _ }
      ~should_traverse
      ~allow_restart =
    let field_name =
      Longident.parse
      @@ String.concat "." [Names.pass_module_name; Ident.field_name ident]
    in
    let project_bottom =
      pexp_field
        ~loc
        (pexp_ident ~loc (Located.lident ~loc Names.bottom_up_arg))
        (Located.mk ~loc field_name)
    and project_top =
      pexp_field
        ~loc
        (pexp_ident ~loc (Located.lident ~loc Names.top_down_arg))
        (Located.mk ~loc field_name)
    in
    let fn_name = Ident.transform_fn_name ident in
    let fn_ident = pexp_ident ~loc (Located.lident ~loc fn_name)
    and traverse_ident =
      let txt = lident @@ Ident.traverse_fn_name ident in
      pexp_ident ~loc (Located.mk ~loc txt)
    in
    let (restart_pat, continue_pat, stop_pat, continue_stop_pat) =
      let elem_pat = ppat_var ~loc (Located.mk ~loc "elem") in
      let (cont_pat, stop_pat) =
        match Restart.encoding allow_restart with
        | Restart.Encode_as_variant ->
          ( ppat_variant ~loc Names.continue_variant_label @@ Some elem_pat,
            ppat_variant ~loc Names.stop_variant_label @@ Some elem_pat )
        | _ ->
          ( ppat_construct ~loc (Located.lident ~loc "Ok") @@ Some elem_pat,
            ppat_construct ~loc (Located.lident ~loc "Error") @@ Some elem_pat
          )
      in
      let restart_pat =
        ppat_variant ~loc Names.restart_variant_label @@ Some elem_pat
      in
      (restart_pat, cont_pat, stop_pat, ppat_or ~loc cont_pat stop_pat)
    in
    let rest_match =
      match allow_restart with
      | Restart.Allow ->
        [%expr
          match [%e project_bottom] with
          | None -> elem
          | Some bu ->
            (match bu elem ~ctx with
            | (_ctx, [%p continue_stop_pat]) -> elem
            | (_ctx, [%p restart_pat]) ->
              [%e fn_ident] elem ~ctx ~top_down ~bottom_up)]
      | Restart.(Disallow _) ->
        [%expr
          match [%e project_bottom] with
          | None -> elem
          | Some bu ->
            (match bu elem ~ctx with
            | (_ctx, [%p continue_stop_pat]) -> elem)]
    in
    let (rest, ctx_pat) =
      if not should_traverse then
        ((fun _ -> rest_match), ppat_var ~loc (Located.mk ~loc "_ctx"))
      else
        ( (fun ctx_expr ->
            [%expr
              let elem =
                [%e traverse_ident] elem ~ctx:[%e ctx_expr] ~top_down ~bottom_up
              in
              [%e rest_match]]),
          ppat_var ~loc (Located.mk ~loc "td_ctx") )
    in
    let body_expr =
      match allow_restart with
      | Restart.Allow ->
        [%expr
          match [%e project_top] with
          | Some td ->
            (match td elem ~ctx with
            | (_ctx, [%p stop_pat]) -> elem
            | ([%p ctx_pat], [%p continue_pat]) ->
              [%e rest @@ pexp_ident ~loc (Located.lident ~loc "td_ctx")]
            | (_ctx, [%p restart_pat]) ->
              [%e fn_ident] elem ~ctx ~top_down ~bottom_up)
          | _ -> [%e rest @@ pexp_ident ~loc (Located.lident ~loc "ctx")]]
      | Restart.(Disallow _) ->
        [%expr
          match [%e project_top] with
          | Some td ->
            (match td elem ~ctx with
            | (_ctx, [%p stop_pat]) -> elem
            | ([%p ctx_pat], [%p continue_pat]) ->
              [%e rest @@ pexp_ident ~loc (Located.lident ~loc "td_ctx")])
          | _ -> [%e rest @@ pexp_ident ~loc (Located.lident ~loc "ctx")]]
    in
    let elem_pat = ppat_var ~loc (Located.mk ~loc "elem") in
    Gen_fn.gen_str fn_name ty tyvars type_info elem_pat body_expr loc

  let gen_str fld ~should_traverse ~allow_restart =
    match fld with
    | Transform_field.Field def ->
      gen_str_def def ~should_traverse ~allow_restart
    | Transform_field.Unsupported Unsupported.{ loc; kind; _ } ->
      let err =
        match kind with
        | Unsupported.Abstract -> Err.unsupported_abstract loc
        | Unsupported.Open -> Err.unsupported_open loc
      in
      value_binding ~loc ~pat:(ppat_any ~loc) ~expr:(pexp_extension ~loc @@ err)

  let gen_sig_def Transform_field.{ ident; ty; loc; _ } =
    let fn_name = Ident.transform_fn_name ident
    and type_ = Gen_fn.gen_fun_ty ty ~loc in
    psig_value ~loc
    @@ value_description ~loc ~name:(Located.mk ~loc fn_name) ~type_ ~prim:[]

  let gen_sig fld =
    match fld with
    | Transform_field.Field def -> gen_sig_def def
    | Transform_field.Unsupported Unsupported.{ ident; loc; kind } ->
      let fn_name = Ident.transform_fn_name ident in
      let ext =
        match kind with
        | Unsupported.Abstract -> Err.unsupported_abstract loc
        | Unsupported.Open -> Err.unsupported_open loc
      in
      let type_ = ptyp_extension ~loc ext in
      psig_value ~loc
      @@ value_description ~loc ~name:(Located.mk ~loc fn_name) ~type_ ~prim:[]
end

module Gen_traverse = struct
  let gen_core_ty ty ~binding ~opaque_map ~maps ~self_type =
    let rec aux ty binding =
      let loc = ty.ptyp_loc in
      let dflt_pat = ppat_var ~loc (Located.mk ~loc binding) in
      let default = (dflt_pat, None, false) in
      let unsupported =
        (dflt_pat, Some (pexp_extension ~loc @@ Err.unsupported_ty loc), false)
      in
      if Annot.has_opaque_attr ty.ptyp_attributes then
        default
      else
        match ty.ptyp_desc with
        | Ptyp_alias (ty, _) -> aux ty binding
        | Ptyp_arrow (arg_lbl, ty_dom, ty_codom) ->
          aux_arrow arg_lbl ty_dom ty_codom binding loc
        | Ptyp_constr ({ loc; txt }, tys) -> aux_constr txt tys binding loc maps
        | Ptyp_poly (_, ty) -> aux ty binding
        | Ptyp_tuple tys -> aux_tuple tys binding loc
        | Ptyp_variant (row_flds, closedflag, lbl_opts) ->
          aux_variant row_flds closedflag lbl_opts ~binding ~loc ~ty
        | Ptyp_any
        | Ptyp_var _ ->
          default
        | _ -> unsupported
    and aux_variant row_flds closed_flag lbl_opts ~binding ~loc ~ty =
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let has_lbl =
        match lbl_opts with
        | None -> (fun _ -> true)
        | Some lbls -> (fun lbl -> List.exists String.(equal lbl.txt) lbls)
      in
      let pat_expr_self_opts =
        List.map (aux_row_fld ~binding ~has_lbl ~loc ~ty) row_flds
      in
      let has_self = List.exists (fun (_, _, s) -> s) pat_expr_self_opts in
      let expr_opt =
        let (cases, has_untraversed) =
          List.fold_left
            (fun (cases, has_untraversed) (pat, expr_opt, _) ->
              match expr_opt with
              | None -> (cases, true)
              | Some expr ->
                (case ~lhs:pat ~guard:None ~rhs:expr :: cases, has_untraversed))
            ([], false)
            pat_expr_self_opts
        in
        let empty =
          match cases with
          | [] -> true
          | _ -> false
        in
        if empty then
          None
        else
          let dflt_case_opt =
            let dflt_case =
              case
                ~lhs:pat
                ~guard:None
                ~rhs:(pexp_ident ~loc (Located.lident ~loc binding))
            in
            if has_untraversed then
              Some dflt_case
            else
              match closed_flag with
              | Open -> Some dflt_case
              | Closed -> None
          in
          let cases =
            List.rev
            @@
            match dflt_case_opt with
            | None -> cases
            | Some case -> case :: cases
          in
          let scrut_expr = pexp_ident ~loc (Located.lident ~loc binding) in
          Some (pexp_match ~loc scrut_expr cases)
      in
      (pat, expr_opt, has_self)
    and aux_row_fld row_fld ~binding ~has_lbl ~loc ~ty =
      let default = (ppat_var ~loc (Located.mk ~loc binding), None, false) in
      match row_fld.prf_desc with
      | Rtag (_, _, []) -> default
      | Rtag (lbl, _, _) when not @@ has_lbl lbl -> default
      | Rtag (lbl, _flag, tys) ->
        (match aux (ptyp_tuple ~loc tys) (binding ^ "_elem") with
        | (tuple_pat, (Some _ as tuple_expr), has_self) ->
          let pat = ppat_variant ~loc lbl.txt (Some tuple_pat) in
          let expr = pexp_variant ~loc lbl.txt tuple_expr in
          (pat, Some expr, has_self)
        | _ -> default)
      | Rinherit extend_ty ->
        let binding = binding ^ "_extend" in
        (match aux extend_ty binding with
        | (_pat, Some expr, has_self) ->
          let ty_pat = ppat_type ~loc @@ Core_ty.ctor_longident_exn extend_ty in
          let pat = ppat_alias ~loc ty_pat (Located.mk ~loc binding) in
          let coerce_expr = pexp_coerce ~loc expr None ty in
          (pat, Some coerce_expr, has_self)
        | _ -> default)
    and aux_constr lident tys binding loc maps =
      let flat_lident = Longident.flatten_exn lident in
      match (flat_lident, tys) with
      (* -- Common type constructors ---------------------------------------- *)
      | ((["option"] | ["Option"; "t"]), [ty]) -> aux_option ty binding loc
      | ((["list"] | ["List"; "t"]), [ty]) ->
        aux_functor [%expr Stdlib.List.map] ty binding loc
      | ((["array"] | ["Array"; "t"]), [ty]) ->
        aux_functor [%expr Stdlib.Array.map] ty binding loc
      | ((["lazy_t"] | ["Lazy"; "t"]), [ty]) -> aux_lazy ty binding loc
      | ((["result"] | ["Result"; ("result" | "t")]), [ty_ok; ty_err]) ->
        aux_result ty_ok ty_err binding loc
      | (["ref"], [ty]) -> aux_ref ty binding loc
      (* -- Common primitives ----------------------------------------------- *)
      | ([ty], []) when SSet.mem ty Core_ty.builtin_prims ->
        (ppat_var ~loc (Located.mk ~loc binding), None, false)
      (* -- - *)
      | ([decl_name], _)
        when SMap.exists (fun nm _ -> String.equal nm decl_name) opaque_map ->
        let pat = ppat_var ~loc (Located.mk ~loc binding) in
        let opaque = SMap.find decl_name opaque_map in
        let is_self =
          match self_type with
          | Some s -> String.equal s decl_name
          | None -> false
        in
        let (expr_opt, has_self) =
          if opaque then
            (None, false)
          else if is_self then
            let binding_expr = pexp_ident ~loc (Located.lident ~loc binding) in
            (Some [%expr self [%e binding_expr] ~ctx ~top_down ~bottom_up], true)
          else
            let fn_name = Ident.(transform_fn_name @@ Type decl_name) in
            let fn_expr = pexp_ident ~loc (Located.lident ~loc fn_name) in
            let binding_expr = pexp_ident ~loc (Located.lident ~loc binding) in
            ( Some
                [%expr [%e fn_expr] [%e binding_expr] ~ctx ~top_down ~bottom_up],
              false )
        in
        (pat, expr_opt, has_self)
      | (ids, _) ->
        (* Check if this is a configured map type *)
        let type_name = String.concat "." ids in
        (match (tys, Maps.find_opt type_name maps) with
        | ([ty], Some map_expr_str) ->
          (* Use configured map function *)
          let map_expr =
            pexp_ident ~loc (Located.mk ~loc (Longident.parse map_expr_str))
          in
          aux_functor map_expr ty binding loc
        | _ ->
          (* Default handling for external types *)
          let pat = ppat_var ~loc (Located.mk ~loc binding) in
          let binding_expr = pexp_ident ~loc (Located.lident ~loc binding) in
          let (ty, path) =
            match List.rev ids with
            | ty :: path -> (ty, path)
            | _ ->
              Location.raise_errorf
                ~loc
                "Invalid type path: %s"
                (String.concat "." ids)
          in
          let identity_lident =
            Longident.parse
            @@ String.concat "."
            @@ List.rev (Names.identity_name :: Names.pass_module_name :: path)
          in
          let identity_expr =
            pexp_ident ~loc (Located.mk ~loc identity_lident)
          in
          let pass_fld_nm = String.concat "_" (Names.pass_field_pfx :: path) in
          let pass_fld =
            {
              loc;
              txt =
                Longident.parse
                @@ String.concat "." [Names.pass_module_name; pass_fld_nm];
            }
          in
          let fn_name =
            let transform_fn = Ident.(transform_fn_name @@ Type ty) in
            Longident.parse
            @@ String.concat "."
            @@ List.rev (transform_fn :: path)
          in
          let fn_expr = pexp_ident ~loc (Located.mk ~loc fn_name)
          and top_down_expr =
            pexp_field
              ~loc
              (pexp_ident ~loc (Located.lident ~loc Names.top_down_arg))
              pass_fld
          and bottom_up_expr =
            pexp_field
              ~loc
              (pexp_ident ~loc (Located.lident ~loc Names.bottom_up_arg))
              pass_fld
          in
          let expr =
            [%expr
              match ([%e top_down_expr], [%e bottom_up_expr]) with
              | (Some top_down, Some bottom_up) ->
                [%e fn_expr] [%e binding_expr] ~ctx ~top_down ~bottom_up
              | (Some top_down, _) ->
                [%e fn_expr]
                  [%e binding_expr]
                  ~ctx
                  ~top_down
                  ~bottom_up:([%e identity_expr] ())
              | (_, Some bottom_up) ->
                [%e fn_expr]
                  [%e binding_expr]
                  ~ctx
                  ~top_down:([%e identity_expr] ())
                  ~bottom_up
              | _ -> [%e binding_expr]]
          in
          (pat, Some expr, false))
    and aux_ref ty binding loc =
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let binding_deref = binding ^ "_deref" in
      let pat_deref = ppat_var ~loc (Located.mk ~loc binding_deref) in
      let expr_elem = pexp_ident ~loc (Located.lident ~loc binding) in
      let (expr_opt, has_self) =
        match aux ty binding_deref with
        | (_pat, Some expr, has_self) ->
          ( Some
              [%expr
                let [%p pat_deref] = ![%e expr_elem] in
                [%e expr_elem] := [%e expr];
                [%e expr_elem]],
            has_self )
        | _ -> (None, false)
      in
      (pat, expr_opt, has_self)
    and aux_lazy ty binding loc =
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let binding_forced = binding ^ "_force" in
      let pat_forced = ppat_var ~loc (Located.mk ~loc binding_forced) in
      let expr_elem = pexp_ident ~loc (Located.lident ~loc binding) in
      let (expr_opt, has_self) =
        match aux ty binding_forced with
        | (_pat, Some expr, has_self) ->
          ( Some
              [%expr
                let [%p pat_forced] = Lazy.force [%e expr_elem] in
                lazy [%e expr]],
            has_self )
        | _ -> (None, false)
      in
      (pat, expr_opt, has_self)
    and aux_option ty binding loc =
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let scrut_expr = pexp_ident ~loc (Located.lident ~loc binding) in
      let (expr_opt, has_self) =
        match aux ty (String.concat "_" [binding; "inner"]) with
        | (pat, Some expr, has_self) ->
          ( Some
              [%expr
                match [%e scrut_expr] with
                | Some [%p pat] -> Some [%e expr]
                | _ -> None],
            has_self )
        | _ -> (None, false)
      in
      (pat, expr_opt, has_self)
    and aux_result ty_ok ty_err binding loc =
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let scrut_expr = pexp_ident ~loc (Located.lident ~loc binding) in
      let (expr_opt, has_self) =
        match
          ( aux ty_ok (String.concat "_" [binding; "ok"]),
            aux ty_err (String.concat "_" [binding; "err"]) )
        with
        | ((pat_ok, Some expr_ok, s1), (pat_err, Some expr_err, s2)) ->
          ( Some
              [%expr
                match [%e scrut_expr] with
                | Ok [%p pat_ok] -> Ok [%e expr_ok]
                | Error [%p pat_err] -> Error [%e expr_err]],
            s1 || s2 )
        | ((pat_ok, Some expr_ok, has_self), _) ->
          ( Some
              [%expr
                match [%e scrut_expr] with
                | Ok [%p pat_ok] -> Ok [%e expr_ok]
                | _ -> [%e scrut_expr]],
            has_self )
        | (_, (pat_err, Some expr_err, has_self)) ->
          ( Some
              [%expr
                match [%e scrut_expr] with
                | Error [%p pat_err] -> Error [%e expr_err]
                | _ -> [%e scrut_expr]],
            has_self )
        | _ -> (None, false)
      in
      (pat, expr_opt, has_self)
    and aux_functor map_expr ty binding loc =
      let (inner_pat, inner_expr_opt, has_self) = aux ty binding in
      let pat = ppat_var ~loc (Located.mk ~loc binding) in
      let arg_expr = pexp_ident ~loc (Located.lident ~loc binding) in
      let default = (pat, None, false) in
      Option.value ~default
      @@ Option.map
           (fun inner_expr ->
             let expr =
               [%expr
                 [%e map_expr]
                   (fun [%p inner_pat] -> [%e inner_expr])
                   [%e arg_expr]]
             in
             (pat, Some expr, has_self))
           inner_expr_opt
    and aux_arrow _arg_lbl _ty_dom _ty_codom binding loc =
      (ppat_var ~loc (Located.mk ~loc binding), None, false)
    and aux_tuple tys binding loc =
      let (pats, expr_res, selfs) =
        List.fold_right
          (fun (pat, expr_res, has_self) (pats, expr_ress, selfs) ->
            (pat :: pats, expr_res :: expr_ress, has_self || selfs))
          (List.mapi
             (fun i ty ->
               let binding = String.concat "_" [binding; string_of_int i] in
               let (pat, expr_opt, has_self) = aux ty binding in
               let expr_res =
                 match expr_opt with
                 | Some expr -> Ok expr
                 | _ -> Error binding
               in
               (pat, expr_res, has_self))
             tys)
          ([], [], false)
      in
      if List.for_all Result.is_error expr_res then
        (ppat_var ~loc (Located.mk ~loc binding), None, false)
      else
        let exprs =
          List.map
            (function
              | Ok expr -> expr
              | Error binding -> pexp_ident ~loc (Located.lident ~loc binding))
            expr_res
        in
        (ppat_tuple ~loc pats, Some (pexp_tuple ~loc exprs), selfs)
    in
    aux ty binding

  let gen_record_field
      Record_field.{ label; ty; _ } ~opaque_map ~maps ~self_type =
    gen_core_ty ~binding:label ty ~opaque_map ~maps ~self_type

  let gen_record_fields
      record_name record_fields ~loc ~opaque_map ~maps ~self_type =
    let fld_opts =
      List.map
        (fun ((Record_field.{ label; loc; _ } as fld), annot_opt) ->
          match annot_opt with
          | Some Annot.Opaque ->
            ((label, loc), (ppat_var ~loc (Located.mk ~loc label), None, false))
          | Some Annot.Explicit ->
            let pat = ppat_var ~loc (Located.mk ~loc label) in
            let fn_nm =
              Ident.(transform_fn_name @@ Field (record_name, label))
            in
            let fn_expr = pexp_ident ~loc (Located.lident ~loc fn_nm)
            and elem_expr = pexp_ident ~loc (Located.lident ~loc label) in
            let expr =
              [%expr [%e fn_expr] [%e elem_expr] ~ctx ~top_down ~bottom_up]
            in

            ((label, loc), (pat, Some expr, false))
          | _ ->
            ((label, loc), gen_record_field fld ~opaque_map ~maps ~self_type))
        record_fields
    in
    let (pats, exprs, partial, empty, has_self) =
      List.fold_right
        (fun ((lbl, loc), (pat, expr_opt, s))
             (pats, exprs, partial, empty, has_self) ->
          let ident = Located.lident ~loc lbl in
          match expr_opt with
          | Some expr ->
            ( (ident, pat) :: pats,
              (ident, expr) :: exprs,
              partial,
              false,
              has_self || s )
          | _ -> (pats, exprs, true, empty, has_self))
        fld_opts
        ([], [], false, true, false)
    in
    if empty then
      (ppat_var ~loc (Located.mk ~loc record_name), None, false)
    else if partial then
      let rcd_pat = ppat_record ~loc pats Open in
      ( ppat_alias ~loc rcd_pat (Located.mk ~loc record_name),
        Some
          (pexp_record ~loc exprs
          @@ Some (pexp_ident ~loc (Located.lident ~loc record_name))),
        has_self )
    else
      ( ppat_record ~loc pats Closed,
        Some (pexp_record ~loc exprs None),
        has_self )

  let gen_variant_ctor
      variant_name variant_ctor ~opaque_map ~maps ~explicit ~self_type =
    let open Variant_ctor in
    match variant_ctor with
    | Constant_ctor (lbl, loc) ->
      ( ppat_construct ~loc (Located.lident ~loc lbl) None,
        (if explicit then
          Some (pexp_extension ~loc @@ Err.unsupported_ctor_args_empty loc)
        else
          None),
        false )
    | Single_ctor (lbl, loc, _) when explicit ->
      let pat =
        ppat_construct ~loc (Located.lident ~loc lbl)
        @@ Some (ppat_var ~loc (Located.mk ~loc "elem"))
      in
      let fn_nm = Ident.(transform_fn_name @@ Ctor (variant_name, lbl)) in
      let fn_expr = pexp_ident ~loc (Located.lident ~loc fn_nm)
      and elem_expr = pexp_ident ~loc (Located.lident ~loc "elem") in
      let apply_expr =
        [%expr [%e fn_expr] [%e elem_expr] ~ctx ~top_down ~bottom_up]
      in
      let expr =
        pexp_construct ~loc (Located.lident ~loc lbl) @@ Some apply_expr
      in
      (pat, Some expr, false)
    | Single_ctor (lbl, loc, ty) ->
      let binding = String.(concat "_" [lowercase_ascii lbl; "elem"]) in
      let (ty_pat, ty_expr_opt, has_self) =
        gen_core_ty ~binding ty ~opaque_map ~maps ~self_type
      in
      let pat = ppat_construct ~loc (Located.lident ~loc lbl) @@ Some ty_pat in
      let expr_opt =
        Option.map
          (fun expr ->
            pexp_construct ~loc (Located.lident ~loc lbl) @@ Some expr)
          ty_expr_opt
      in
      (pat, expr_opt, has_self)
    | Tuple_ctor (lbl, loc, tys) when explicit ->
      let tuple_pat =
        ppat_tuple ~loc
        @@ List.mapi
             (fun i _ ->
               let name = "elem_" ^ string_of_int i in
               ppat_var ~loc (Located.mk ~loc name))
             tys
      in
      let pat =
        ppat_construct ~loc (Located.lident ~loc lbl) @@ Some tuple_pat
      in
      let fn_nm = Ident.(transform_fn_name @@ Ctor (variant_name, lbl)) in
      let fn_expr = pexp_ident ~loc (Located.lident ~loc fn_nm)
      and elem_expr =
        pexp_tuple ~loc
        @@ List.mapi
             (fun i _ ->
               pexp_ident ~loc (Located.lident ~loc ("elem_" ^ string_of_int i)))
             tys
      in
      let construct_expr =
        pexp_construct ~loc (Located.lident ~loc lbl) @@ Some elem_expr
      in
      let expr =
        [%expr
          let [%p tuple_pat] =
            [%e fn_expr] [%e elem_expr] ~ctx ~top_down ~bottom_up
          in
          [%e construct_expr]]
      in
      (pat, Some expr, false)
    | Tuple_ctor (lbl, loc, tys) ->
      let ty = ptyp_tuple ~loc tys in
      let binding = String.(concat "_" [lowercase_ascii lbl; "elem"]) in
      let (ty_pat, ty_expr_opt, has_self) =
        gen_core_ty ~binding ty ~opaque_map ~maps ~self_type
      in
      let pat = ppat_construct ~loc (Located.lident ~loc lbl) @@ Some ty_pat in
      let expr_opt =
        Option.map
          (fun expr ->
            pexp_construct ~loc (Located.lident ~loc lbl) @@ Some expr)
          ty_expr_opt
      in
      (pat, expr_opt, has_self)
    | Record_ctor (lbl, loc, flds) ->
      let binding = String.lowercase_ascii lbl in
      let (ty_pat, ty_expr_opt, has_self) =
        gen_record_fields binding flds ~loc ~opaque_map ~maps ~self_type
      in
      let pat = ppat_construct ~loc (Located.lident ~loc lbl) @@ Some ty_pat in
      if explicit then
        (pat, Some (pexp_extension ~loc @@ Err.unsupported_ctor_args loc), false)
      else
        let expr_opt =
          Option.map
            (fun expr ->
              pexp_construct ~loc (Located.lident ~loc lbl) @@ Some expr)
            ty_expr_opt
        in
        (pat, expr_opt, has_self)

  let gen_variant_ctors
      variant_name variant_ctors ~loc ~opaque_map ~maps ~self_type =
    let elem lbl = String.(concat "_" [lowercase_ascii lbl; "elem"]) in
    let ctor_opts =
      List.map
        (fun (ctor, annot_opt) ->
          let (loc, txt) =
            let open Variant_ctor in
            match ctor with
            | Constant_ctor (lbl, loc) -> (loc, elem lbl)
            | Single_ctor (lbl, loc, _) -> (loc, elem lbl)
            | Tuple_ctor (lbl, loc, _) -> (loc, elem lbl)
            | Record_ctor (lbl, loc, _) -> (loc, elem lbl)
          in
          match annot_opt with
          | Some Annot.Opaque ->
            ((txt, loc), (ppat_var ~loc (Located.mk ~loc txt), None, false))
          | Some Annot.Explicit ->
            ( (txt, loc),
              gen_variant_ctor
                variant_name
                ctor
                ~opaque_map
                ~maps
                ~explicit:true
                ~self_type )
          | _ ->
            ( (txt, loc),
              gen_variant_ctor
                variant_name
                ctor
                ~opaque_map
                ~maps
                ~explicit:false
                ~self_type ))
        variant_ctors
    in
    let (pats, exprs, partial, empty, has_self) =
      List.fold_right
        (fun (_, (pat, expr_opt, s)) (pats, exprs, partial, empty, has_self) ->
          match expr_opt with
          | Some expr ->
            (pat :: pats, expr :: exprs, partial, false, has_self || s)
          | _ -> (pats, exprs, true, empty, has_self))
        ctor_opts
        ([], [], false, true, false)
    in
    let pat_variant_nm = ppat_var ~loc (Located.mk ~loc variant_name)
    and exp_variant_nm = pexp_ident ~loc (Located.lident ~loc variant_name) in
    if empty then
      (pat_variant_nm, None, false)
    else
      let cases =
        let named =
          List.map2 (fun lhs rhs -> case ~lhs ~guard:None ~rhs) pats exprs
        in
        if partial then
          named @ [case ~lhs:pat_variant_nm ~guard:None ~rhs:exp_variant_nm]
        else
          named
      in
      let expr = pexp_match ~loc exp_variant_nm cases in
      (pat_variant_nm, Some expr, has_self)

  let gen_def
      Transform_field.{ ident; ty; loc; definition; tyvars; type_info; _ }
      ~opaque_map
      ~maps =
    let self_type =
      match ident with
      | Ident.Type name -> Some name
      | Ident.Ctor _
      | Ident.Field _ ->
        None
    in
    let (pat, expr_opt, has_self_calls) =
      match definition with
      | Transform_field.Core_ty def_ty ->
        gen_core_ty
          def_ty
          ~opaque_map
          ~maps
          ~self_type
          ~binding:(Ident.to_string ident)
      | Transform_field.Variant_ctors (name, ctors) ->
        gen_variant_ctors name ctors ~loc ~opaque_map ~maps ~self_type
      | Transform_field.Record_fields (name, flds) ->
        gen_record_fields name flds ~loc ~opaque_map ~maps ~self_type
    in
    let fn_name = Ident.traverse_fn_name ident in
    Option.map
      (fun body_expr ->
        let body_expr =
          if has_self_calls then
            let field_name =
              Longident.parse
              @@ String.concat
                   "."
                   [Names.pass_module_name; Ident.field_name ident]
            in
            let project_top =
              pexp_field
                ~loc
                (pexp_ident ~loc (Located.lident ~loc Names.top_down_arg))
                (Located.mk ~loc field_name)
            and project_bottom =
              pexp_field
                ~loc
                (pexp_ident ~loc (Located.lident ~loc Names.bottom_up_arg))
                (Located.mk ~loc field_name)
            in
            let traverse_expr = pexp_ident ~loc (Located.lident ~loc fn_name)
            and transform_expr =
              pexp_ident
                ~loc
                (Located.lident ~loc (Ident.transform_fn_name ident))
            in
            [%expr
              let self =
                match ([%e project_top], [%e project_bottom]) with
                | (None, None) -> [%e traverse_expr]
                | _ -> [%e transform_expr]
              in
              [%e body_expr]]
          else
            body_expr
        in
        Gen_fn.gen_str fn_name ty tyvars type_info pat body_expr loc)
      expr_opt

  let gen_str fld ~opaque_map ~maps =
    match fld with
    | Transform_field.Field def -> gen_def def ~opaque_map ~maps
    | Transform_field.Unsupported Unsupported.{ loc; kind; _ } ->
      let err =
        match kind with
        | Unsupported.Abstract -> Err.unsupported_abstract loc
        | Unsupported.Open -> Err.unsupported_open loc
      in
      Some
        (value_binding
           ~loc
           ~pat:(ppat_any ~loc)
           ~expr:(pexp_extension ~loc @@ err))
end

let gen_str ~loc ~path:_ (_rec_flag, tds) restart maps =
  let allow_restart = Option.value ~default:Restart.Allow restart in
  let maps = Option.value ~default:Maps.empty maps in
  let analysis = Analyse.analyse tds ~maps in
  let strat_transform_fields = Transform_field.fields analysis in
  let pass_fields = Pass_field.fields analysis in
  let transform_fields = List.concat strat_transform_fields in
  let pass_ty_decl =
    Gen_pass.gen_str transform_fields pass_fields ~loc ~allow_restart
  in
  let combine_fn =
    Gen_combine.gen_str transform_fields pass_fields ~loc ~allow_restart
  in
  let identity = Gen_identity.gen_str transform_fields pass_fields ~loc in
  let fns =
    List.map
      (fun flds ->
        let vbs =
          List.concat_map
            (fun tfld ->
              match
                Gen_traverse.gen_str
                  tfld
                  ~opaque_map:analysis.Analyse.opaque_map
                  ~maps:analysis.Analyse.maps
              with
              | Some vb ->
                [
                  vb;
                  Gen_transform.gen_str
                    tfld
                    ~should_traverse:true
                    ~allow_restart;
                ]
              | _ ->
                [
                  Gen_transform.gen_str
                    tfld
                    ~should_traverse:false
                    ~allow_restart;
                ])
            flds
        in
        let recursive =
          if List.length vbs = 1 && not (Restart.allowed allow_restart) then
            Nonrecursive
          else
            Recursive
        in
        pstr_value ~loc recursive vbs)
      strat_transform_fields
  in
  let pass_module =
    let name = Located.mk ~loc (Some Names.pass_module_name)
    and expr = pmod_structure ~loc [pass_ty_decl; identity; combine_fn] in
    pstr_module ~loc @@ module_binding ~loc ~name ~expr
  in
  pass_module :: fns

let gen_sig ~loc ~path:_ (_rec_flag, tds) restart maps =
  let allow_restart = Option.value ~default:Restart.Allow restart in
  let maps = Option.value ~default:Maps.empty maps in
  let analysis = Analyse.analyse tds ~maps in
  let transform_fields = List.concat @@ Transform_field.fields analysis in
  let pass_fields = Pass_field.fields analysis in
  let pass_ty =
    Gen_pass.gen_sig transform_fields pass_fields ~loc ~allow_restart
  in
  let combine_fn = Gen_combine.gen_sig loc in
  let identity = Gen_identity.gen_sig loc in
  let fns = List.map Gen_transform.gen_sig transform_fields in
  let pass_module =
    let name = Located.mk ~loc (Some Names.pass_module_name)
    and type_ = pmty_signature ~loc [pass_ty; combine_fn; identity] in
    psig_module ~loc @@ module_declaration ~loc ~name ~type_
  in
  pass_module :: fns

let parse_maps_expr expr =
  let extract_string e =
    match e.pexp_desc with
    | Pexp_constant (Pconst_string (s, _, _)) -> s
    | _ ->
      Location.raise_errorf ~loc:e.pexp_loc "~maps: expected a string literal"
  in
  let extract_entry e =
    match e.pexp_desc with
    | Pexp_constant (Pconst_string (s, _, _)) -> (s, Maps.infer_map_fn s)
    | Pexp_tuple [k; v] -> (extract_string k, extract_string v)
    | _ ->
      Location.raise_errorf
        ~loc:e.pexp_loc
        "~maps: expected a string or pair (\"Type.t\", \"Module.map\")"
  in
  let rec extract_list e =
    match e.pexp_desc with
    | Pexp_construct ({ txt = Lident "[]"; _ }, None) -> []
    | Pexp_construct ({ txt = Lident "::"; _ }, Some cons) ->
      (match cons.pexp_desc with
      | Pexp_tuple [hd; tl] -> extract_entry hd :: extract_list tl
      | _ ->
        Location.raise_errorf ~loc:e.pexp_loc "~maps: expected a list of pairs")
    | _ ->
      Location.raise_errorf ~loc:e.pexp_loc "~maps: expected a list literal"
  in
  Maps.of_list (extract_list expr)

let args () =
  let inner =
    Ast_pattern.(
      alt
        (as__ @@ pexp_variant (string "Encode_as_variant") none
        |> map1 ~f:(fun _ -> Restart.(Disallow Encode_as_variant)))
        (as__ @@ pexp_variant (string "Encode_as_result") none
        |> map1 ~f:(fun _ -> Restart.(Disallow Encode_as_result))))
  in
  let restart_pat =
    Ast_pattern.(
      alt
        (as__ @@ pexp_variant (string "Allow") none
        |> map1 ~f:(fun _ -> Restart.Allow))
        (pexp_variant (string "Disallow") (some inner)))
  in
  let maps_pat = Ast_pattern.(__ |> map1 ~f:parse_maps_expr) in
  Deriving.Args.(empty +> arg "restart" restart_pat +> arg "maps" maps_pat)

let transform =
  Deriving.add
    Names.transform_pfx
    ~str_type_decl:Deriving.(Generator.make (args ()) gen_str)
    ~sig_type_decl:Deriving.(Generator.make (args ()) gen_sig)
