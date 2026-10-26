(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
open Typing_class_types
module SN = Naming_special_names

type class_t = Typing_class_types.class_t [@@deriving show]

let make x = x

module ApiShallow = struct
  let abstract (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Abstract @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_abstract

  let final (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Final @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_final

  let has_const_attribute (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Const @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_const

  let kind (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Kind @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_kind

  let allow_multiple_instantiations (_decl, (c, _), _ctx) =
    c.Decl_defs.dc_allow_multiple_instantiations

  let is_xhp (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Is_xhp @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_is_xhp

  let name (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Name @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_name

  let pos (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Pos @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_pos

  let tparams (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Tparams @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_tparams

  let enum_type (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Enum_type @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_enum_type

  let xhp_enum_values (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Xhp_enum_values @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_xhp_enum_values

  let xhp_marked_empty (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Xhp_marked_empty @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_xhp_marked_empty

  let sealed_whitelist (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Sealed_whitelist @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_sealed_whitelist

  let get_docs_url (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Docs_url @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_docs_url

  let get_module (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Module @@ fun () ->
    let (c, _) = t in
    Option.map c.Decl_defs.dc_module ~f:snd

  let get_package (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Module @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_package

  let internal (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Internal @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_internal

  let tests_bypass_visibility (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.TestsBypassVisibility
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_tests_bypass_visibility

  let allow_unchecked_enum_values (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.AllowUncheckedEnumValues
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_allow_unchecked_enum_values

  let is_module_level_trait (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.IsModuleLevelTrait
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_is_module_level_trait

  let decl_errors (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Decl_errors @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_decl_errors

  let get_support_dynamic_type (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Support_dynamic_type
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_support_dynamic_type
end

module ApiLazy = struct
  let construct (decl, t, ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Construct @@ fun () ->
    let (cls, members) = t in
    match !(members.construct) with
    | Some x -> x
    | None ->
      let x =
        Decl_class.lookup_constructor_lazy
          ctx
          ~child_class_name:cls.Decl_defs.dc_name
          cls.Decl_defs.dc_substs
          cls.Decl_defs.dc_construct
      in
      members.construct := Some x;
      x

  let need_init (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Need_init @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_need_init

  let get_ancestor (decl, t, _ctx) ancestor =
    Decl_counters.count_subdecl decl (Decl_counters.Get_ancestor ancestor)
    @@ fun () ->
    let (c, _) = t in
    S_map.find_opt ancestor c.Decl_defs.dc_ancestors

  let has_ancestor (decl, t, _ctx) ancestor =
    Decl_counters.count_subdecl decl (Decl_counters.Has_ancestor ancestor)
    @@ fun () ->
    let (c, _) = t in
    S_map.mem ancestor c.Decl_defs.dc_ancestors

  let requires_ancestor (decl, t, _ctx) ancestor =
    Decl_counters.count_subdecl decl (Decl_counters.Requires_ancestor ancestor)
    @@ fun () ->
    let (c, _) = t in
    S_set.mem ancestor c.Decl_defs.dc_req_ancestors_extends

  let get_const (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_const id) @@ fun () ->
    let (c, _) = t in
    S_map.find_opt id c.Decl_defs.dc_consts

  let has_const (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_const id) @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_consts

  let get_typeconst (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_typeconst id)
    @@ fun () ->
    let (c, _) = t in
    S_map.find_opt id c.Decl_defs.dc_typeconsts

  let has_typeconst (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_typeconst id)
    @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_typeconsts

  let get_typeconst_enforceability (decl, t, _ctx) id =
    Decl_counters.count_subdecl
      decl
      (Decl_counters.Get_typeconst_enforceability id)
    @@ fun () ->
    let (c, _) = t in
    Option.map (S_map.find_opt id c.Decl_defs.dc_typeconsts) ~f:(fun t ->
        t.ttc_enforceable)

  let get_prop (decl, t, ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_prop id) @@ fun () ->
    let (c, members) = t in
    match Hashtbl.find members.props id with
    | Some _ as elt_opt -> elt_opt
    | None ->
      (match S_map.find_opt id c.Decl_defs.dc_props with
      | None -> None
      | Some elt ->
        let elt = Decl_class.lookup_property_type_lazy ctx c id elt in
        Hashtbl.add_exn members.props ~key:id ~data:elt;
        Some elt)

  let has_prop (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_prop id) @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_props

  let get_sprop (decl, t, ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_sprop id) @@ fun () ->
    let (c, members) = t in
    match Hashtbl.find members.static_props id with
    | Some _ as elt_opt -> elt_opt
    | None ->
      (match S_map.find_opt id c.Decl_defs.dc_sprops with
      | None -> None
      | Some elt ->
        let elt = Decl_class.lookup_static_property_type_lazy ctx c id elt in
        Hashtbl.add_exn members.static_props ~key:id ~data:elt;
        Some elt)

  let has_sprop (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_sprop id) @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_sprops

  let get_method (decl, t, ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_method id) @@ fun () ->
    let (c, members) = t in
    match Hashtbl.find members.methods id with
    | Some _ as elt_opt -> elt_opt
    | None ->
      (match S_map.find_opt id c.Decl_defs.dc_methods with
      | None -> None
      | Some elt ->
        let elt = Decl_class.lookup_method_type_lazy ctx c id elt in
        Hashtbl.add_exn members.methods ~key:id ~data:elt;
        Some elt)

  let has_method (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_method id) @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_methods

  let get_smethod (decl, t, ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Get_smethod id) @@ fun () ->
    let (c, members) = t in
    match Hashtbl.find members.static_methods id with
    | Some _ as elt_opt -> elt_opt
    | None ->
      (match S_map.find_opt id c.Decl_defs.dc_smethods with
      | None -> None
      | Some elt ->
        let elt = Decl_class.lookup_static_method_type_lazy ctx c id elt in
        Hashtbl.add_exn members.static_methods ~key:id ~data:elt;
        Some elt)

  let has_smethod (decl, t, _ctx) id =
    Decl_counters.count_subdecl decl (Decl_counters.Has_smethod id) @@ fun () ->
    let (c, _) = t in
    S_map.mem id c.Decl_defs.dc_smethods

  let get_any_method ~is_static cls id =
    (* tally is already done inside the following three methods *)
    if String.equal id SN.Members.__construct then
      fst (construct cls)
    else if is_static then
      get_smethod cls id
    else
      get_method cls id
end

module ApiEager = struct
  let all_ancestor_req_names (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.All_ancestor_req_names
    @@ fun () ->
    (* The two below will traverse ancestors in different orders.
       But if the typechecker discovers errors in different order, no matter. *)
    let (c, _) = t in
    S_set.elements c.Decl_defs.dc_req_ancestors_extends

  let all_ancestors (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.All_ancestors @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_ancestors

  let all_ancestor_names (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.All_ancestors @@ fun () ->
    let (c, _) = t in
    S_map.ordered_keys c.Decl_defs.dc_ancestors

  let all_ancestor_reqs (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.All_ancestor_reqs
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_req_ancestors

  let all_ancestor_req_constraints_requirements (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.All_ancestor_reqs
    @@ fun () ->
    let (c, _) = t in
    c.Decl_defs.dc_req_constraints_ancestors

  let all_ancestor_req_class_requirements (decl, t, _ctx) =
    List.filter_map
      (all_ancestor_req_constraints_requirements (decl, t, _ctx))
      ~f:(fun r ->
        match r with
        | CR_Equal c -> Some c
        | CR_Subtype _ -> None)

  let all_ancestor_req_this_as_requirements (decl, t, _ctx) =
    List.filter_map
      (all_ancestor_req_constraints_requirements (decl, t, _ctx))
      ~f:(fun r ->
        match r with
        | CR_Equal _ -> None
        | CR_Subtype c -> Some c)

  let upper_bounds_on_this t =
    (* tally is already done by all_ancestors and upper_bounds *)
    List.map ~f:(fun req -> snd req) (all_ancestor_reqs t)
    |> List.append
         (List.map
            ~f:(fun cr -> snd (to_requirement cr))
            (all_ancestor_req_constraints_requirements t))

  let consts (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Consts @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_consts

  let typeconsts (decl, t, _ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Typeconsts @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_typeconsts

  let props (decl, t, ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Props @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_props
    |> List.map ~f:(fun (id, elt) ->
           (id, Decl_class.lookup_property_type_lazy ctx c id elt))

  let sprops (decl, t, ctx) =
    Decl_counters.count_subdecl decl Decl_counters.SProps @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_sprops
    |> List.map ~f:(fun (id, elt) ->
           (id, Decl_class.lookup_static_property_type_lazy ctx c id elt))

  let methods (decl, t, ctx) =
    Decl_counters.count_subdecl decl Decl_counters.Methods @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_methods
    |> List.map ~f:(fun (id, elt) ->
           (id, Decl_class.lookup_method_type_lazy ctx c id elt))

  let smethods (decl, t, ctx) =
    Decl_counters.count_subdecl decl Decl_counters.SMethods @@ fun () ->
    let (c, _) = t in
    S_map.bindings c.Decl_defs.dc_smethods
    |> List.map ~f:(fun (id, elt) ->
           (id, Decl_class.lookup_static_method_type_lazy ctx c id elt))

  let overridden_method (decl, t, ctx) ~method_name ~is_static ~get_class =
    let open Option.Monad_infix in
    Decl_counters.count_subdecl decl Decl_counters.Overridden_method
    @@ fun () ->
    ctx >>= fun ctx ->
    let get_method (ty : decl_ty) : class_elt option =
      let (_, (_, class_name), _) = Decl_utils.unwrap_class_type ty in
      get_class ctx class_name >>= fun cls ->
      ApiLazy.get_any_method ~is_static cls method_name
    in
    let (cls, _members) = t in
    Decl_provider_internals.get_shallow_class ctx cls.Decl_defs.dc_name
    >>= Decl_inherit.find_overridden_method ~get_method

  let get_static_member_from_req_constraints
      (decl, t, ctx) ~get_member ~get_class =
    let open Option.Monad_infix in
    Decl_counters.count_subdecl decl Decl_counters.All_ancestor_reqs
    @@ fun () ->
    ctx >>= fun ctx ->
    let req_constraints =
      List.map
        (all_ancestor_req_constraints_requirements (decl, t, Some ctx))
        ~f:(fun cr -> snd (to_requirement cr))
    in
    List.find_map req_constraints ~f:(fun req_ty ->
        match get_node req_ty with
        | Tapply ((_, cn), _) ->
          get_class ctx cn >>= fun req_class -> get_member req_class
        | _ -> None)

  let get_smethod_from_req_constraints cls method_name ~get_class =
    get_static_member_from_req_constraints
      cls
      ~get_member:(fun req_class -> ApiLazy.get_smethod req_class method_name)
      ~get_class

  let get_sprop_from_req_constraints cls prop_name ~get_class =
    get_static_member_from_req_constraints
      cls
      ~get_member:(fun req_class -> ApiLazy.get_sprop req_class prop_name)
      ~get_class
end

type t =
  (Decl_counters.decl option[@opaque])
  * class_t
  * (Provider_context.t[@opaque]) option
[@@deriving show]

include ApiShallow
include ApiLazy
include ApiEager

let deferred_init_members (decl, t, _ctx) =
  Decl_counters.count_subdecl decl Decl_counters.Deferred_init_members
  @@ fun () ->
  let (c, _) = t in
  c.Decl_defs.dc_deferred_init_members
