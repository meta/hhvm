(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(*****************************************************************************)
(* Module used when we want to figure out what has changed.
 * 1) The user modifies a file
 * 2) We compute the type of the file (cf typing_redecl_service.ml)
 * 3) We compare the old type and the new type of the class (or function)
 *    to see if anything has changed. This is where the code of this module
 *    comes into play. This code compares the old and the new class type
 *    and computes the set of dependencies if something needs to be
 *    either redeclared or checked again.
 *)
(*****************************************************************************)
open Hh_prelude
open Decl_defs
open Typing_deps

module VersionedNames = struct
  type t = {
    old_names: File_info.Old_names.t;
    new_names: File_info.names;
  }
  [@@deriving show]

  let empty : t =
    { old_names = File_info.Old_names.empty; new_names = File_info.empty_names }

  let make_unchanged (names : File_info.names) : t =
    { old_names = File_info.Old_names.of_names names; new_names = names }

  let merge left right : t =
    let { old_names = left_old_names; new_names = left_new_names } = left in
    let { old_names = right_old_names; new_names = right_new_names } = right in
    {
      old_names = File_info.Old_names.merge left_old_names right_old_names;
      new_names = File_info.merge_names left_new_names right_new_names;
    }
end

module VersionedSSet = struct
  type t = {
    old: S_set.t;
    new_: S_set.t;
  }

  type diff = {
    removed: S_set.t;
    kept: S_set.t;
    added: S_set.t;
  }

  let empty = { old = S_set.empty; new_ = S_set.empty }

  let project (project : File_info.names -> S_set.t) (names : VersionedNames.t)
      : t =
    let { VersionedNames.old_names; new_names } = names in
    {
      old = project (File_info.Old_names.to_names old_names);
      new_ = project new_names;
    }

  let get_classes = project (fun names -> names.File_info.n_classes)

  let merge (left : t) (right : t) : t =
    let { old = old_left; new_ = new_left } = left in
    let { old = old_right; new_ = new_right } = right in
    {
      old = S_set.union old_left old_right;
      new_ = S_set.union new_left new_right;
    }

  let empty_diff =
    { removed = S_set.empty; kept = S_set.empty; added = S_set.empty }

  let diff { old; new_ } : diff =
    let diff =
      S_set.fold
        (fun old_name diff ->
          match S_set.find_opt old_name new_ with
          | None -> { diff with removed = S_set.add old_name diff.removed }
          | Some n -> { diff with kept = S_set.add n diff.kept })
        old
        empty_diff
    in
    { diff with added = S_set.diff new_ old }

  let diff_cardinal { removed; kept; added } =
    S_set.cardinal removed + S_set.cardinal kept + S_set.cardinal added
end

module VersionedFileInfo = struct
  type t = {
    funs: VersionedSSet.t;
    types: VersionedSSet.t;
    gconsts: VersionedSSet.t;
    modules: VersionedSSet.t;
  }

  let transpose (names : VersionedNames.t) : t =
    let { VersionedNames.old_names; new_names } = names in
    let {
      File_info.n_funs = old_funs;
      n_classes = _;
      n_types = old_types;
      n_consts = old_gconsts;
      n_modules = old_modules;
    } =
      File_info.Old_names.to_names old_names
    in
    let {
      File_info.n_funs = new_funs;
      n_classes = _;
      n_types = new_types;
      n_consts = new_gconsts;
      n_modules = new_modules;
    } =
      new_names
    in
    {
      funs = { VersionedSSet.old = old_funs; new_ = new_funs };
      types = { VersionedSSet.old = old_types; new_ = new_types };
      gconsts = { VersionedSSet.old = old_gconsts; new_ = new_gconsts };
      modules = { VersionedSSet.old = old_modules; new_ = new_modules };
    }

  module Diff = struct
    type t = {
      funs: VersionedSSet.diff;
      types: VersionedSSet.diff;
      gconsts: VersionedSSet.diff;
      modules: VersionedSSet.diff;
    }
  end

  let diff (t : t) : Diff.t =
    let { funs; types; gconsts; modules } = t in
    {
      Diff.funs = VersionedSSet.diff funs;
      types = VersionedSSet.diff types;
      gconsts = VersionedSSet.diff gconsts;
      modules = VersionedSSet.diff modules;
    }

  let diff_names (names : VersionedNames.t) : Diff.t =
    names |> transpose |> diff
end

(*****************************************************************************)
(* Given two classes give back the set of functions or classes that need
 * to be rechecked
 *)
(*****************************************************************************)
module Class_diff = struct
  let smap_left s1 s2 =
    S_map.fold
      begin
        fun x ty1 diff ->
          let ty2 = S_map.find_opt x s2 in
          match ty2 with
          | Some ty2 ->
            if Poly.( = ) ty1 ty2 then
              diff
            else
              S_set.add x diff
          | None -> S_set.add x diff
      end
      s1
      S_set.empty

  let smap s1 s2 = S_set.union (smap_left s1 s2) (smap_left s2 s1)

  let compare class1 class2 =
    let is_unchanged = true in
    (* compare class constants *)
    let consts_diff = smap class1.dc_consts class2.dc_consts in
    let is_unchanged = is_unchanged && S_set.is_empty consts_diff in
    (* compare class members *)
    let props_diff = smap class1.dc_props class2.dc_props in
    let is_unchanged = is_unchanged && S_set.is_empty props_diff in
    (* compare class static members *)
    let sprops_diff = smap class1.dc_sprops class2.dc_sprops in
    let is_unchanged = is_unchanged && S_set.is_empty sprops_diff in
    (* compare class methods *)
    let methods_diff = smap class1.dc_methods class2.dc_methods in
    let is_unchanged = is_unchanged && S_set.is_empty methods_diff in
    (* compare class static methods *)
    let smethods_diff = smap class1.dc_smethods class2.dc_smethods in
    let is_unchanged = is_unchanged && S_set.is_empty smethods_diff in
    (* compare class constructors *)
    let ctor_diff = Poly.( <> ) class1.dc_construct class2.dc_construct in
    let is_unchanged = is_unchanged && not ctor_diff in
    (* compare class type constants *)
    let typeconsts_diff = smap class1.dc_typeconsts class2.dc_typeconsts in
    let is_unchanged = is_unchanged && S_set.is_empty typeconsts_diff in
    is_unchanged
end

(*****************************************************************************)
(* Given two classes give back the set of functions or classes that need
 * to be rechecked because the type of their member changed
 *)
(*****************************************************************************)
module Class_elt_diff = struct
  open Decl_heap

  let acc_diff is_unchanged xmap =
    let is_unchanged =
      match is_unchanged with
      | `Unchanged when not @@ S_map.is_empty xmap -> `Changed
      | x -> x
    in
    is_unchanged

  let diff_elts
      (type t)
      (module EltHeap : Shared_mem.Heap
        with type key = string * string
         and type value = t)
      ~cid
      ~elts1
      ~elts2
      ~normalize =
    S_map.merge
      begin
        fun name elt1 elt2 ->
          let key = (cid, name) in
          let match1 =
            match elt1 with
            | Some elt -> String.equal elt.elt_origin cid
            | _ -> false
          in
          let match2 =
            match elt2 with
            | Some elt -> String.equal elt.elt_origin cid
            | _ -> false
          in
          if match1 || match2 then
            match (EltHeap.get_old key, EltHeap.get key) with
            | (None, _)
            | (_, None) ->
              Some ()
            | (Some x1, Some x2) ->
              let ty1 = normalize x1 in
              let ty2 = normalize x2 in
              if Poly.( = ) ty1 ty2 then
                None
              else
                Some ()
          else
            None
      end
      elts1
      elts2

  let compare_props class1 class2 acc =
    let cid = class1.dc_name in
    let (elts1, elts2) = (class1.dc_props, class2.dc_props) in
    let diff =
      diff_elts
        (module Props)
        ~cid
        ~elts1
        ~elts2
        ~normalize:Decl_pos_utils.NormalizeSig.ty
    in
    acc_diff acc diff

  let compare_sprops class1 class2 acc =
    let cid = class1.dc_name in
    let (elts1, elts2) = (class1.dc_sprops, class2.dc_sprops) in
    let diff =
      diff_elts
        (module StaticProps)
        ~cid
        ~elts1
        ~elts2
        ~normalize:Decl_pos_utils.NormalizeSig.ty
    in
    acc_diff acc diff

  let compare_meths class1 class2 acc =
    let cid = class1.dc_name in
    let (elts1, elts2) = (class1.dc_methods, class2.dc_methods) in
    let diff =
      diff_elts
        (module Methods)
        ~cid
        ~elts1
        ~elts2
        ~normalize:Decl_pos_utils.NormalizeSig.fun_elt
    in
    acc_diff acc diff

  let compare_smeths class1 class2 acc =
    let cid = class1.dc_name in
    let (elts1, elts2) = (class1.dc_smethods, class2.dc_smethods) in
    let diff =
      diff_elts
        (module StaticMethods)
        ~cid
        ~elts1
        ~elts2
        ~normalize:Decl_pos_utils.NormalizeSig.fun_elt
    in
    acc_diff acc diff

  let compare_cstrs class1 class2 =
    let cid = class1.dc_name in
    let match1 =
      match class1.dc_construct with
      | (Some elt, _) -> String.equal elt.elt_origin cid
      | _ -> false
    in
    let match2 =
      match class2.dc_construct with
      | (Some elt, _) -> String.equal elt.elt_origin cid
      | _ -> false
    in
    if match1 || match2 then
      match (Constructors.get_old cid, Constructors.get cid) with
      | (None, _)
      | (_, None) ->
        `Changed
      | (Some fe1, Some fe2) ->
        let fe1 = Decl_pos_utils.NormalizeSig.fun_elt fe1 in
        let fe2 = Decl_pos_utils.NormalizeSig.fun_elt fe2 in
        if Poly.( = ) fe1 fe2 then
          `Unchanged
        else
          `Changed
    else
      `Unchanged

  let compare class1 class2 =
    compare_cstrs class1 class2
    |> compare_props class1 class2
    |> compare_sprops class1 class2
    |> compare_meths class1 class2
    |> compare_smeths class1 class2
end

let get_extend_deps mode cid_hash acc =
  Typing_deps.get_extend_deps
    ~mode
    ~visited:(VisitedSet.make ())
    ~source_class:cid_hash
    ~acc

let add_fanout make_dep mode id fanout_acc =
  Fanout.add_fanout_of mode (make_dep id) fanout_acc

let add_fun_fanout = add_fanout (fun id -> Dep.Fun id)

let add_type_fanout = add_fanout (fun id -> Dep.Type id)

let add_gconst_fanout mode id fanout =
  fanout
  |> add_fanout (fun id -> Dep.GConst id) mode id
  |> add_fanout (fun id -> Dep.GConstName id) mode id

let add_module_fanout = add_fanout (fun id -> Dep.Module id)

(*****************************************************************************)
(* Determine which functions/classes have to be rechecked after comparing
 * the old and the new type signature of "fid" (function identifier).
 *)
(*****************************************************************************)

let get_fun_deps ~ctx ~mode old_funs fid (fanout_acc, old_funs_missing) =
  match
    ( S_map.find fid old_funs,
      match Provider_backend.get () with
      | Provider_backend.Rust_provider_backend backend ->
        Rust_provider_backend.Decl.get_fun
          backend
          (Naming_provider.rust_backend_ctx_proxy ctx)
          fid
      | _ -> Decl_heap.Funs.get fid )
  with
  (* Note that we must include all dependencies even if we get the None, None
   * case. Due to the fact we can declare types lazily, there may be no
   * existing declaration in the old Decl_heap that corresponds to a function
   * `foo` in the AST. Then when the user deletes `foo`, the new Decl_heap
   * will also lack a definition of `foo`. Now we must recheck all the use
   * sites of `foo` to make sure there are no dangling references. *)
  | (None, _)
  | (_, None) ->
    let fanout_acc = add_fun_fanout mode fid fanout_acc in
    (fanout_acc, old_funs_missing + 1)
  | (Some fe1, Some fe2) ->
    let fe1 = Decl_pos_utils.NormalizeSig.fun_elt fe1 in
    let fe2 = Decl_pos_utils.NormalizeSig.fun_elt fe2 in
    if Poly.( = ) fe1 fe2 then
      (fanout_acc, old_funs_missing)
    else
      let fanout_acc = add_fun_fanout mode fid fanout_acc in
      (fanout_acc, old_funs_missing)

let get_funs_deps ~ctx old_funs (funs : VersionedSSet.diff) =
  let mode = Provider_context.get_deps_mode ctx in
  let { VersionedSSet.removed; kept; added } = funs in
  let fanout =
    Fanout.empty
    |> S_set.fold (add_fun_fanout mode) removed
    |> S_set.fold (add_fun_fanout mode) added
  in
  S_set.fold (get_fun_deps ~ctx ~mode old_funs) kept (fanout, 0)

(*****************************************************************************)
(* Determine which functions/classes have to be rechecked after comparing
 * the old and the new typedef
 *)
(*****************************************************************************)
let get_type_deps ~ctx ~mode old_types tid (fanout_acc, old_types_missing) =
  match
    ( S_map.find tid old_types,
      match Provider_backend.get () with
      | Provider_backend.Rust_provider_backend backend ->
        Rust_provider_backend.Decl.get_typedef
          backend
          (Naming_provider.rust_backend_ctx_proxy ctx)
          tid
      | _ -> Decl_heap.Typedefs.get tid )
  with
  | (None, _)
  | (_, None) ->
    let fanout_acc = add_type_fanout mode tid fanout_acc in
    (fanout_acc, old_types_missing + 1)
  | (Some tdef1, Some tdef2) ->
    let tdef1 = Decl_pos_utils.NormalizeSig.typedef tdef1 in
    let tdef2 = Decl_pos_utils.NormalizeSig.typedef tdef2 in
    let is_same_signature = Poly.( = ) tdef1 tdef2 in
    if is_same_signature then
      (fanout_acc, old_types_missing)
    else
      let fanout_acc = add_type_fanout mode tid fanout_acc in
      (fanout_acc, old_types_missing)

let get_types_deps ~ctx old_types (types : VersionedSSet.diff) =
  let mode = Provider_context.get_deps_mode ctx in
  let { VersionedSSet.removed; kept; added } = types in
  let fanout =
    Fanout.empty
    |> S_set.fold (add_type_fanout mode) removed
    |> S_set.fold (add_type_fanout mode) added
  in
  S_set.fold (get_type_deps ~ctx ~mode old_types) kept (fanout, 0)

(*****************************************************************************)
(* Determine which top level definitions have to be rechecked if the constant
 * changed.
 *)
(*****************************************************************************)
let get_gconst_deps
    ~ctx ~mode old_gconsts cst_id (fanout_acc, old_gconsts_missing) =
  let cst1 = S_map.find cst_id old_gconsts in
  let cst2 =
    match Provider_backend.get () with
    | Provider_backend.Rust_provider_backend backend ->
      Rust_provider_backend.Decl.get_gconst
        backend
        (Naming_provider.rust_backend_ctx_proxy ctx)
        cst_id
    | _ -> Decl_heap.GConsts.get cst_id
  in
  match (cst1, cst2) with
  | (None, _)
  | (_, None) ->
    let fanout_acc = add_gconst_fanout mode cst_id fanout_acc in
    (fanout_acc, old_gconsts_missing + 1)
  | (Some cst1, Some cst2) ->
    let is_same_signature = Poly.( = ) cst1 cst2 in
    if is_same_signature then
      (fanout_acc, old_gconsts_missing)
    else
      let dep = Dep.GConst cst_id in
      let fanout_acc = Fanout.add_fanout_of mode dep fanout_acc in
      (fanout_acc, old_gconsts_missing)

let get_gconsts_deps ~ctx old_gconsts gconsts =
  let mode = Provider_context.get_deps_mode ctx in
  let { VersionedSSet.removed; kept; added } = gconsts in
  let fanout =
    Fanout.empty
    |> S_set.fold (add_gconst_fanout mode) removed
    |> S_set.fold (add_gconst_fanout mode) added
  in
  S_set.fold (get_gconst_deps ~ctx ~mode old_gconsts) kept (fanout, 0)

let get_module_deps ~ctx ~mode old_modules mid (fanout_acc, old_modules_missing)
    : Fanout.t * int =
  match
    ( S_map.find mid old_modules,
      match Provider_backend.get () with
      | Provider_backend.Rust_provider_backend backend ->
        Rust_provider_backend.Decl.get_module
          backend
          (Naming_provider.rust_backend_ctx_proxy ctx)
          mid
      | _ -> Decl_heap.Modules.get mid )
  with
  | (None, _)
  | (_, None) ->
    let fanout_acc = add_module_fanout mode mid fanout_acc in
    (fanout_acc, old_modules_missing + 1)
  | (Some _, Some _) -> (fanout_acc, old_modules_missing)

let get_modules_deps ~ctx ~old_modules ~modules =
  let mode = Provider_context.get_deps_mode ctx in
  let { VersionedSSet.removed; kept; added } = modules in
  let fanout =
    Fanout.empty
    |> S_set.fold (add_module_fanout mode) removed
    |> S_set.fold (add_module_fanout mode) added
  in
  S_set.fold (get_module_deps ~ctx ~mode old_modules) kept (fanout, 0)
