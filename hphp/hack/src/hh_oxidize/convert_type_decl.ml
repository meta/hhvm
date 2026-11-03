(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Asttypes
open Longident
open Parsetree
open Printf
open Utils
open Output
open State
open Convert_longident
open Rust_type

(* Types marked with [@@oxidize.exclude] are not generated in Rust.
   GADT constructors whose return type instantiates a type parameter
   with an excluded type are silently filtered out. *)
let excluded_types : S_set.t ref = ref S_set.empty

let reset_excluded_types () = excluded_types := S_set.empty

let add_excluded_type name = excluded_types := S_set.add name !excluded_types

let is_excluded_type name = S_set.mem name !excluded_types

let has_oxidize_exclude_attr (attrs : attributes) : bool =
  List.exists attrs ~f:(fun { attr_name; attr_payload; _ } ->
      match (attr_name, attr_payload) with
      | ({ txt = "oxidize.exclude"; _ }, PStr []) -> true
      | _ -> false)

let rec collect_type_names_from_core_type (ct : core_type) : string list =
  match ct.ptyp_desc with
  | Ptyp_constr ({ txt = Lident name; _ }, args) ->
    name :: List.concat_map args ~f:collect_type_names_from_core_type
  | Ptyp_constr (_, args) ->
    List.concat_map args ~f:collect_type_names_from_core_type
  | Ptyp_tuple tys -> List.concat_map tys ~f:collect_type_names_from_core_type
  | _ -> []

(** Check if a GADT constructor's return type instantiates a type parameter
    with an excluded type. For example, [Tvar : Tvid.t -> locl_phase ty_]
    has [pcd_res = locl_phase ty_], and if [locl_phase] is excluded, this
    constructor is filtered out. *)
let pcd_res_references_excluded_type (cd : constructor_declaration) : bool =
  match cd.pcd_res with
  | None -> false
  | Some ct ->
    (match ct.ptyp_desc with
    | Ptyp_constr (_, args) ->
      List.exists args ~f:(fun arg ->
          List.exists
            (collect_type_names_from_core_type arg)
            ~f:is_excluded_type)
    | _ -> false)

let stringify_attribute { attr_name; attr_payload; _ } =
  match (attr_name, attr_payload) with
  | ({ txt = "ocaml.doc" | "value" | "oxidize.exclude"; _ }, _) -> None
  | ({ txt; _ }, PStr []) -> Some txt
  | ({ txt; _ }, PStr [structure_item]) ->
    let item =
      structure_item
      |> Format.asprintf "%a" Pprintast.structure_item
      |> String.strip ~drop:(function
             | ' '
             | '\t'
             | ';' ->
               true
             | _ -> false)
    in
    Some (txt ^ " " ^ item)
  | _ -> None

let add_default_attr_if_ocaml_yojson_drop_if attributes acc_attr_list =
  let contains_yojson_drop_if attr =
    match stringify_attribute attr with
    | None -> false
    | Some attr -> String.is_prefix attr ~prefix:"yojson_drop_if"
  in
  if List.exists attributes ~f:contains_yojson_drop_if then
    "default" :: acc_attr_list
  else
    acc_attr_list

let add_deserialize_with_arena tys acc_attr_list =
  let contains_ref = List.exists ~f:Rust_type.contains_ref tys in
  if contains_ref then
    (* deserialize a type contains any Cell causes a compilation error, see T90211775 *)
    let contains_cell =
      List.exists
        ~f:(fun t ->
          Rust_type.type_name_and_params t
          |> fst
          |> String.is_suffix ~suffix:"::Cell")
        tys
    in
    if contains_cell then
      "skip" :: acc_attr_list
    else
      let acc_attr_list =
        if contains_ref then
          "borrow" :: acc_attr_list
        else
          acc_attr_list
      in
      "deserialize_with = \"arena_deserializer::arena\"" :: acc_attr_list
  else
    acc_attr_list

let rust_de_field_attr (tys : Rust_type.t list) (attributes : attributes) :
    string =
  let serde_attr_list =
    []
    |> add_default_attr_if_ocaml_yojson_drop_if attributes
    |> add_deserialize_with_arena tys
  in
  if List.is_empty serde_attr_list then
    ""
  else
    sprintf "#[serde(%s)]" @@ String.concat ~sep:", " serde_attr_list

let default_implements () = []

let implements_traits _name = default_implements ()

let default_derives () =
  [
    (Some "ocamlrep", "FromOcamlRep");
    (None, "Clone");
    (None, "Debug");
    (None, "Eq");
    (None, "Hash");
    (None, "Ord");
    (None, "PartialEq");
    (None, "PartialOrd");
    (Some "no_pos_hash", "NoPosHash");
    (Some "eq_modulo_pos", "EqModuloPos");
    (Some "ocamlrep", "ToOcamlRep");
    (Some "serde", "Serialize");
    (Some "serde", "Deserialize");
  ]

let derive_copy ty = Convert_type.is_copy (Rust_type.rust_simple_type ty)

let derive_default (ty : label) =
  List.mem
    [
      "map_reduce_ffi::MapReduceFfi";
      "package_info::PackageInfo";
      "scoured_comments::ScouredComments";
      "tast_collector::ByNames";
      "tast_hashes::ByNames";
      "tast_hashes::FileInfo";
      "user_error_flags::UserErrorFlags";
    ]
    ty
    ~equal:String.equal

let additional_derives ty : (string option * string) list =
  let result = [] in
  let result =
    if derive_copy ty then
      (None, "Copy") :: (Some "ocamlrep", "FromOcamlRepIn") :: result
    else
      result
  in
  let result =
    if derive_default ty then
      (None, "Default") :: result
    else
      result
  in
  let result =
    let strum = Some "strum" in
    if String.equal ty "experimental_features::FeatureName" then
      (strum, "Display")
      :: (strum, "EnumIter")
      :: (strum, "EnumString")
      :: (strum, "IntoStaticStr")
      :: result
    else if String.equal ty "experimental_features::FeatureStatus" then
      (strum, "EnumString") :: result
    else if String.equal ty "typing_deps_kind::DepKind" then
      (strum, "EnumString") :: (strum, "FromRepr") :: result
    else
      result
  in
  result

module DeriveSkipLists : sig
  val skip_derive : ty:string -> trait:string -> bool
end = struct
  let skip_list_for_ty ty =
    match ty with
    (* A custom implementation of Ord for UserDiagnostic matches the sorting behavior of
       diagnostics in OCaml. *)
    | "user_diagnostic::UserDiagnostic" -> ["Ord"; "PartialOrd"]
    (* These types contain or embed floats, so they only implement PartialEq/PartialOrd,
       not Eq/Hash/Ord. *)
    | "hh_config::HhConfig"
    | "global_options::GlobalOptions"
    | "edenfs_watcher_types::Changes" ->
      ["Eq"; "EqModuloPos"; "Hash"; "NoPosHash"; "Ord"]
    (* And Global_options is used in Genv which is used in Env. We
     * don't care about comparison or hashing on environments *)
    | "typing_env_types::Env" ->
      ["Eq"; "EqModuloPos"; "Hash"; "NoPosHash"; "Ord"]
    | "typing_env_types::Genv" ->
      ["Eq"; "EqModuloPos"; "Hash"; "NoPosHash"; "Ord"]
    (* And Global_options is used in SavedEnv. *)
    | "tast::SavedEnv" -> ["Eq"; "EqModuloPos"; "Hash"; "NoPosHash"; "Ord"]
    | "tast::ByNames" -> ["Eq"; "EqModuloPos"; "Hash"; "NoPosHash"; "Ord"]
    | "ast_defs::Id" -> ["Debug"]
    | "typing_defs_core::Ty" -> ["Eq"; "PartialEq"; "Ord"; "PartialOrd"]
    | "typing_defs_core::Ty_" -> ["Debug"]
    | "file_info::Change" -> ["EqModuloPos"]
    | _ -> []

  let skip_list_for_trait trait =
    match trait with
    | "EqModuloPos" ->
      [
        "scoured_comments::*";
        "pos_or_decl::*";
        "namespace_env::*";
        "file_info::NameType";
        "file_info::Pos";
        "file_info::Id";
        "file_info::Ids";
        "file_info::FileInfo";
        "file_info::Names";
        "file_info::SavedNames";
        "file_info::Saved";
        "file_info::Diff";
        "aast_defs::*";
        "nast::*";
        "tast::*";
        "full_fidelity_parser_env::*";
        "lints_core::*";
        "typing_env_types::*";
        "typing_tyvar_occurrences::*";
        "typing_per_cont_env::*";
        "typing_inference_env::*";
        "typing_kinding_defs::*";
        "type_parameter_env::*";
        "typing_fake_members::*";
        "typing_defs_core::HasMember";
        "typing_defs_core::Destructure";
        "typing_defs_core::DestructureKind";
        "typing_defs_core::CanIndex";
        "typing_defs_core::CanIndexAssign";
        "typing_defs_core::ConstraintType_";
        "typing_defs_core::ConstraintType";
        "typing_defs_core::InternalType";
      ]
    | _ -> []

  let is_in_ty_skip_list ~ty ~trait =
    List.mem (skip_list_for_ty ty) trait ~equal:String.equal

  let is_in_trait_skip_list ~ty ~trait =
    let path_ty = String.split ty ~on:':' in
    List.exists (skip_list_for_trait trait) ~f:(fun skip_ty ->
        (* if skip_ty is like "SomeTy" then treat it as unqualified
         * and skip if any type like "some_path::SomeTy" is in the
         * skip list. Otherwise, just compare the fully qualified types,
         * modulo "*". *)
        match String.split skip_ty ~on:':' with
        | [skip_ty] ->
          (match List.last path_ty with
          | None -> false
          | Some ty -> String.equal ty skip_ty)
        | path_skip_ty ->
          List.equal
            (fun node skip_node ->
              String.equal node skip_node || String.equal "*" skip_node)
            path_ty
            path_skip_ty)

  let skip_derive ~ty ~trait =
    is_in_ty_skip_list ~ty ~trait || is_in_trait_skip_list ~ty ~trait
end

let derived_traits ty =
  let ty = sprintf "%s::%s" (curr_module_name ()) ty in
  default_derives ()
  |> List.filter ~f:(fun (_, trait) ->
         not (DeriveSkipLists.skip_derive ~ty ~trait))
  |> List.append (additional_derives ty)

let denylisted_types () =
  [
    ("aast_defs", "ByteString");
    ("diagnostics", "FinalizedError");
    ("diagnostics", "FinalizedDiagnostic");
    ("diagnostics", "Marker");
    ("diagnostics", "MarkedMessage");
    ("diagnostics", "PositionGroup");
    ("file_info", "Saved");
    ("typing_defs", "ExpandEnv");
    ("typing_defs", "PhaseTy");
    ("typing_defs", "WildcardAction");
    ("typing_reason", "DeclPhase");
    ("typing_reason", "LoclPhase");
    ("typing_reason", "Stats");
    ("typing_reason", "PathElem");
    ("typing_reason", "Path");
  ]

(* HACK: ignore anything beginning with the "decl" or "locl" prefix, since the
   oxidized version of Ty does not have a phase. *)
let denylisted_type_prefixes =
  [
    ("typing_defs", "Decl");
    ("typing_defs_core", "Decl");
    ("typing_defs", "Locl");
    ("typing_defs_core", "Locl");
  ]

(* HACK: Typing_reason is usually aliased to Reason, so we have lots of
   instances of Reason.t. Since we usually convert an identifier like Reason.t
   to reason::Reason, the actual type needs to be renamed to the common alias.
   This looks nicer anyway. *)
let renamed_types = [(("typing_reason", "TypingReason"), "Reason")]

(* By default, when we see an alias to a tuple type, we will assume the alias
   adds some meaning, and generate a new tuple struct type named after the
   alias. In some cases, the alias adds no meaning and we should also use an
   alias in Rust. *)
let tuple_aliases =
  [
    ("ast_defs", "Pstring");
    ("ast_defs", "PositionedByteString");
    ("diagnostics", "Message");
    ("typing_reason", "PosId");
  ]

let newtypes =
  [
    ("aast_defs", "Block");
    ("aast_defs", "FinallyBlock");
    ("aast_defs", "Program");
    ("aast_defs", "UserAttributes");
    ("file_info", "HashType");
  ]

(*
A list of (<module>, <ty1>) where ty1 is enum and all non-empty variant fields should
be wrapped by Box to keep ty1 size down.
*)
let box_variant () =
  [
    ("aast_defs", "Expr_");
    ("aast_defs", "Stmt_");
    ("aast_defs", "Def");
    ("aast_defs", "Pattern");
  ]

let equal_s2 = [%derive.eq: string * string]

let should_box_variant ty =
  List.mem (box_variant ()) (curr_module_name (), ty) ~equal:equal_s2

(* When should_box_variant returns true, we will switch to boxing the fields of
   each variant by default. Some fields are small enough not to need boxing,
   though, so we opt out of the boxing behavior for them here to avoid
   unnecessary indirections. The rule of thumb I'm using here is that the size
   should be two words or less (the size of a slice). *)
let unbox_field ty =
  let open String in
  let ty = Rust_type.rust_type_to_string ty in
  ty = "String"
  || ty = "bstr::BString"
  || is_prefix ty ~prefix:"Vec<"
  || is_prefix ty ~prefix:"Block<"
  || is_prefix ty ~prefix:"&'a "
  || is_prefix ty ~prefix:"Option<&'a "
  || is_prefix ty ~prefix:"std::cell::Cell<&'a "
  || is_prefix ty ~prefix:"std::cell::RefCell<&'a "

let add_rcoc = [("aast_defs", "Nsenv"); ("aast", "Nsenv")]

let should_add_rcoc ty =
  List.mem add_rcoc (curr_module_name (), ty) ~equal:equal_s2

let denylisted ty_name =
  let ty = (curr_module_name (), ty_name) in
  List.mem (denylisted_types ()) ty ~equal:equal_s2
  || List.exists denylisted_type_prefixes ~f:(fun (mod_name, prefix) ->
         String.equal mod_name (curr_module_name ())
         && String.is_prefix ty_name ~prefix)

let rename ty_name =
  List.find renamed_types ~f:(fun (x, _) ->
      equal_s2 x (curr_module_name (), ty_name))
  |> Option.value_map ~f:snd ~default:ty_name

let should_use_alias_instead_of_tuple_struct ty_name =
  let equal = [%derive.eq: string * string] in
  List.mem tuple_aliases (curr_module_name (), ty_name) ~equal

let should_be_newtype ty_name =
  let equal = [%derive.eq: string * string] in
  List.mem newtypes (curr_module_name (), ty_name) ~equal

let doc_comment_of_attribute { attr_name; attr_payload; _ } =
  match (attr_name, attr_payload) with
  | ({ txt = "ocaml.doc"; _ }, PStr structure_items) ->
    List.find_map structure_items ~f:(fun structure_item ->
        match structure_item.pstr_desc with
        | Pstr_eval
            ({ pexp_desc = Pexp_constant (Pconst_string (doc, _, _)); _ }, _) ->
          Some doc
        | _ -> None)
  | _ -> None

let convert_doc_comment doc =
  doc
  |> String.strip ~drop:(function
         | '*'
         | ' '
         | '\n'
         | '\t' ->
           true
         | _ -> false)
  |> String.split ~on:'\n'
  |> List.fold
       ~init:(false, [])
       ~f:(fun (was_in_code_block, lines) original_line ->
         (* Remove leading whitespace *)
         let lstripped = String.lstrip original_line in
         let maybe_chop_prefix prefix s =
           String.chop_prefix s ~prefix |> Option.value ~default:s
         in
         (* Remove leading asterisk and one space after, if present *)
         let no_asterisk =
           lstripped |> maybe_chop_prefix "*" |> maybe_chop_prefix " "
         in
         let now_in_code_block =
           if String.is_prefix ~prefix:"```" (String.lstrip no_asterisk) then
             not was_in_code_block
           else
             was_in_code_block
         in
         let line =
           if
             now_in_code_block
             && was_in_code_block
             && String.equal lstripped no_asterisk
           then
             sprintf "///%s\n" original_line
           else
             sprintf "/// %s\n" no_asterisk
         in
         (now_in_code_block, line :: lines))
  |> (fun (_, l) -> List.rev l)
  |> String.concat

let rec find_type_in_mli_signature (signature : signature) (type_name : label) :
    type_declaration option =
  match signature with
  | [] -> None
  | { Parsetree.psig_desc = Psig_type (_, type_decls); _ } :: rest ->
    (match
       List.find type_decls ~f:(fun td ->
           String.equal td.Parsetree.ptype_name.txt type_name)
     with
    | Some type_decl -> Some type_decl
    | None -> find_type_in_mli_signature rest type_name)
  | _ :: rest -> find_type_in_mli_signature rest type_name

let find_constructor (type_decl : type_declaration) (variant_name : label) :
    constructor_declaration option =
  match type_decl.Parsetree.ptype_kind with
  | Ptype_variant constructors ->
    List.find constructors ~f:(fun cd ->
        String.equal cd.Parsetree.pcd_name.txt variant_name)
  | _ -> None

let find_record_field_in_mli_type_decl mli_type_decl field_name =
  match mli_type_decl.Parsetree.ptype_kind with
  | Ptype_record labels ->
    List.find labels ~f:(fun ld ->
        String.equal ld.Parsetree.pld_name.txt field_name)
  | _ -> None

let doc_comment_of_attribute_list attrs =
  attrs
  |> List.find_map ~f:doc_comment_of_attribute
  |> Option.map ~f:convert_doc_comment
  |> Option.value ~default:""

let merge_doc_comments ~(from_ml : string) ~(from_mli : string option) : string
    =
  if
    String.is_empty from_ml
    || String.(strip (lowercase from_ml) = "/// see .mli")
  then
    Option.value from_mli ~default:""
  else
    from_ml

let doc_comment_of_attribute_list_mli_aware
    (attrs : attribute list) (type_name : label) : label =
  let from_ml = doc_comment_of_attribute_list attrs in
  let from_mli =
    let open Option.Let_syntax in
    let* signature = State.curr_mli_signature () in
    let+ mli_type_decl = find_type_in_mli_signature signature type_name in
    doc_comment_of_attribute_list mli_type_decl.ptype_attributes
  in
  merge_doc_comments ~from_ml ~from_mli

let doc_comment_of_variant_constructor
    (attrs : attribute list)
    ~(original_type_name : label)
    ~(constructor : label) : label =
  let from_ml = doc_comment_of_attribute_list attrs in
  let from_mli =
    let open Option.Let_syntax in
    let* signature = State.curr_mli_signature () in
    let* mli_type_decl =
      find_type_in_mli_signature signature original_type_name
    in
    let+ variant_cd = find_constructor mli_type_decl constructor in
    doc_comment_of_attribute_list variant_cd.pcd_attributes
  in
  merge_doc_comments ~from_ml ~from_mli

let doc_comment_of_record_field
    (attrs : attribute list) ~(original_type_name : label) ~(field_name : label)
    : label =
  let from_ml = doc_comment_of_attribute_list attrs in
  let from_mli =
    let open Option.Let_syntax in
    let* signature = State.curr_mli_signature () in
    let* mli_type_decl =
      find_type_in_mli_signature signature original_type_name
    in
    let+ field_ld =
      find_record_field_in_mli_type_decl mli_type_decl field_name
    in
    doc_comment_of_attribute_list field_ld.pld_attributes
  in
  merge_doc_comments ~from_ml ~from_mli

let ocaml_attr attrs =
  attrs
  |> List.filter_map ~f:stringify_attribute
  |> List.map ~f:(fun attr ->
         if String.contains attr '"' then
           Printf.sprintf "#[rust_to_ocaml(attr = r#\"%s\"#)]\n" attr
         else
           Printf.sprintf "#[rust_to_ocaml(attr = \"%s\")]\n" attr)
  |> String.concat ~sep:""

let type_param (ct, _) = Convert_type.core_type ct

let type_params ~safe_ints _name params =
  let params = List.map ~f:(type_param ~safe_ints) params in
  let lifetime = [] in
  (lifetime, params)

let record_label_declaration
    ?(pub = false)
    ?(prefix = "")
    ~safe_ints
    ~original_type_name
    (ld : label_declaration) : label =
  let doc =
    doc_comment_of_record_field
      ld.pld_attributes
      ~original_type_name
      ~field_name:ld.pld_name.txt
  in
  let attr = ocaml_attr ld.pld_attributes in
  let pub =
    if pub then
      "pub "
    else
      ""
  in
  let name =
    ld.pld_name.txt |> String.chop_prefix_exn ~prefix |> convert_field_name
  in
  let ty =
    match
      List.find ld.pld_attributes ~f:(fun { attr_name; _ } ->
          String.equal attr_name.txt "oxidize.usize")
    with
    | None -> Convert_type.core_type ~safe_ints ld.pld_type
    | Some { attr_payload = PStr []; _ } ->
      (match ld.pld_type.ptyp_desc with
      | Ptyp_constr ({ txt = Lident "int"; _ }, []) -> rust_simple_type "usize"
      | _ ->
        raise (Skip_type_decl "[@oxidize.usize] requires an int record field"))
    | Some _ ->
      raise (Skip_type_decl "[@oxidize.usize] does not accept a payload")
  in
  sprintf
    "%s%s%s%s %s: %s,\n"
    doc
    (rust_de_field_attr [ty] ld.pld_attributes)
    attr
    pub
    name
    (rust_type_to_string ty)

let find_record_label_prefix labels =
  let prefix =
    labels |> List.map ~f:(fun ld -> ld.pld_name.txt) |> common_prefix_of_list
  in
  (* Only remove a common prefix up to the last underscore (if a record has
     fields x_bar and x_baz, we want to remove x_, not x_ba). *)
  let idx = ref (String.length prefix) in
  while !idx > 0 && Char.(prefix.[!idx - 1] <> '_') do
    idx := !idx - 1
  done;
  String.sub prefix ~pos:0 ~len:!idx

let record_prefix_attr prefix =
  if String.is_empty prefix then
    ""
  else
    sprintf "#[rust_to_ocaml(prefix = \"%s\")]\n" prefix

let declare_record_arguments
    ?(pub = false) ~safe_ints ~prefix ~original_type_name labels =
  labels
  |> map_and_concat
       ~f:(record_label_declaration ~safe_ints ~pub ~prefix ~original_type_name)
  |> sprintf "{\n%s}"

let declare_constructor_arguments ?(box_fields = false) ~safe_ints types :
    Rust_type.t list =
  if not box_fields then
    if List.is_empty types then
      []
    else
      List.map ~f:(Convert_type.core_type ~safe_ints) types
  else
    match types with
    | [] -> []
    | [ty] ->
      let ty = Convert_type.core_type ~safe_ints ty in
      let ty =
        if unbox_field ty then
          ty
        else
          rust_type "Box" [] [ty]
      in
      [ty]
    | _ -> [rust_type "Box" [] [Convert_type.tuple ~safe_ints types]]

let variant_constructor_value cd =
  (* If we see the [@value 42] attribute, assume it's for ppx_deriving enum,
     and that all the variants are zero-argument (i.e., assume this is a
     C-like enum and provide custom discriminant values). *)
  List.find_map cd.pcd_attributes ~f:(fun { attr_name; attr_payload; _ } ->
      match (attr_name, attr_payload) with
      | ( { txt = "value"; _ },
          PStr
            [
              {
                pstr_desc =
                  Pstr_eval
                    ( {
                        pexp_desc =
                          Pexp_constant (Pconst_integer (discriminant, None));
                        _;
                      },
                      _ );
                _;
              };
            ] ) ->
        Some discriminant
      | _ -> None)

let variant_constructor_declaration
    ?(box_fields = false) ~safe_ints ~original_type_name cd =
  let doc =
    doc_comment_of_variant_constructor
      cd.pcd_attributes
      ~original_type_name
      ~constructor:cd.pcd_name.txt
  in
  let attr = ocaml_attr cd.pcd_attributes in
  let name = convert_type_name cd.pcd_name.txt in
  let name_attr =
    if String.equal name cd.pcd_name.txt then
      ""
    else
      sprintf "#[rust_to_ocaml(name = \"%s\")]\n" cd.pcd_name.txt
  in
  let value =
    variant_constructor_value cd
    |> Option.value_map ~f:(( ^ ) " = ") ~default:""
  in
  match cd.pcd_args with
  | Pcstr_tuple types ->
    let tys = declare_constructor_arguments ~box_fields ~safe_ints types in
    sprintf
      "%s%s%s%s%s%s%s%s,\n"
      doc
      (rust_de_field_attr tys cd.pcd_attributes)
      attr
      name_attr
      (if box_fields && List.length types > 1 then
        "#[rust_to_ocaml(inline_tuple)]"
      else
        "")
      name
      (if List.is_empty tys then
        ""
      else
        map_and_concat ~sep:"," ~f:rust_type_to_string tys |> sprintf "(%s)")
      value
  | Pcstr_record labels ->
    let prefix = find_record_label_prefix labels in
    sprintf
      "%s%s%s%s%s%s%s,\n"
      doc
      attr
      (record_prefix_attr prefix)
      name_attr
      name
      (declare_record_arguments ~safe_ints labels ~prefix ~original_type_name)
      value

let ctor_arg_len (ctor_args : constructor_arguments) : int =
  match ctor_args with
  | Pcstr_tuple x -> List.length x
  | Pcstr_record x -> List.length x

(* When converting a variant type to a Rust enum, consider whether the enum will
   be "C-like" (i.e., a type where all variants take no arguments), and if so,
   what the maximum [@value] annotation was. *)
type enum_kind =
  | C_like of {
      max_value: int;
      num_variants: int;
    }
  | Sum_type of { num_variants: int }
  | Not_an_enum

let type_declaration ~mutual_rec ~safe_ints ~original_type_name name td =
  let tparam_list =
    match (td.ptype_params, td.ptype_name.txt) with
    (* HACK: eliminate tparam from `type _ ty_` and phase-parameterized types *)
    | ([({ ptyp_desc = Ptyp_any; _ }, _)], "ty_")
    | ([({ ptyp_desc = Ptyp_var "phase"; _ }, _)], _)
    | ([({ ptyp_desc = Ptyp_var "ty"; _ }, _)], _)
      when String.(
             curr_module_name () = "typing_defs_core"
             || curr_module_name () = "typing_defs") ->
      []
    | ([({ ptyp_desc = Ptyp_any; _ }, _)], "witness")
      when String.(curr_module_name () = "typing_reason") ->
      []
    | ([({ ptyp_desc = Ptyp_any; _ }, _)], "t_")
      when String.(curr_module_name () = "typing_reason") ->
      []
    | (tparams, _) -> tparams
  in
  let (lifetime, tparams) = type_params ~safe_ints name tparam_list in
  let serde_attr =
    if List.is_empty lifetime || List.is_empty tparams then
      ""
    else
      let bounds =
        map_and_concat
          ~sep:", "
          ~f:(fun v ->
            sprintf
              "%s: 'de + arena_deserializer::DeserializeInArena<'de>"
              (Rust_type.rust_type_to_string v))
          tparams
      in
      sprintf "#[serde(bound(deserialize = \"%s\" ))]" bounds
  in
  let doc =
    doc_comment_of_attribute_list_mli_aware
      td.ptype_attributes
      original_type_name
  in
  let attr = ocaml_attr td.ptype_attributes in
  let attr =
    if mutual_rec then
      "#[rust_to_ocaml(and)]\n" ^ attr
    else
      attr
  in
  let attrs_and_vis ?(additional_attrs = "") enum_kind ~force_derive_copy =
    if
      force_derive_copy
      && Configuration.is_known (Configuration.copy_type name) false
    then
      failwith
        (Printf.sprintf
           "Type %s::%s can implement Copy but is not specified in the copy_types.txt and/or extern_types.txt file. Please add it."
           (curr_module_name ())
           name);
    let additional_derives =
      if force_derive_copy then
        [(None, "Copy")]
      else
        []
    in
    let derive_attr =
      let traits = derived_traits name @ additional_derives in
      let traits =
        match enum_kind with
        | C_like _ -> (Some "ocamlrep", "FromOcamlRep") :: traits
        | _ -> traits
      in
      let traits =
        if force_derive_copy then
          (Some "ocamlrep", "FromOcamlRepIn") :: traits
        else
          traits
      in
      traits
      |> List.dedup_and_sort ~compare:(fun (_, t1) (_, t2) ->
             String.compare t1 t2)
      |> List.map ~f:(fun (m, trait) ->
             Option.iter m ~f:(fun m -> add_extern_use (m ^ "::" ^ trait));
             trait)
      |> String.concat ~sep:", "
      |> sprintf "#[derive(%s)]"
    in
    let repr =
      match enum_kind with
      | C_like { max_value; num_variants }
        when max num_variants (max_value + 1) <= 256 ->
        "\n#[repr(u8)]"
      | Sum_type { num_variants } when num_variants <= 256 -> "\n#[repr(C, u8)]"
      | _ -> "\n#[repr(C)]"
    in
    let additional_attrs =
      if
        String.equal name "FeatureName"
        && String.equal (curr_module_name ()) "experimental_features"
      then
        "#[strum(serialize_all = \"snake_case\")]\n" ^ additional_attrs
      else
        additional_attrs
    in
    doc ^ derive_attr ^ serde_attr ^ attr ^ additional_attrs ^ repr ^ "\npub"
  in
  let deserialize_in_arena_macro ~force_derive_copy =
    if force_derive_copy || String.equal name "EmitId" then
      let lts = List.map lifetime ~f:(fun _ -> Rust_type.lifetime "arena") in
      sprintf
        "arena_deserializer::impl_deserialize_in_arena!(%s%s);\n"
        name
        (type_params_to_string lts tparams)
    else
      ""
  in
  let implements ~force_derive_copy =
    let traits = implements_traits name in
    let traits =
      if force_derive_copy then
        (Some "arena_trait", "TrivialDrop") :: traits
      else
        traits
    in
    traits
    |> List.dedup_and_sort ~compare:(fun (_, t1) (_, t2) ->
           String.compare t1 t2)
    |> List.map ~f:(fun (m, trait) ->
           Option.iter m ~f:(fun m -> add_extern_use (m ^ "::" ^ trait));
           trait)
    |> List.map ~f:(fun trait ->
           sprintf
             "\nimpl%s %s for %s%s {}"
             (type_params_to_string ~bound:trait lifetime tparams)
             trait
             name
             (type_params_to_string lifetime tparams))
    |> String.concat ~sep:""
  in
  match (td.ptype_kind, td.ptype_manifest) with
  | (_, Some ty) ->
    (* The manifest represents a `= <some_type>` clause. When td.ptype_kind is
       Ptype_abstract, this is a simple type alias:

         type foo = Other_module.bar

       In this case, the manifest contains the type Other_module.bar.

       The ptype_kind can also be a full type definition. It is Ptype_variant in
       a declaration like this:

         type foo = Other_module.foo =
            | Bar
            | Baz

       For these declarations, the OCaml compiler verifies that the variants in
       Other_module.foo are equivalent to the ones we define in this
       Ptype_variant.

       I don't think there's a direct equivalent to this in Rust, or any reason
       to try to reproduce it. If we see a manifest, we can ignore the
       ptype_kind and just alias, re-export, or define a newtype for
       Other_module.foo. *)
    (match ty.ptyp_desc with
    (* Polymorphic variants. *)
    | Ptyp_variant _ ->
      raise (Skip_type_decl "polymorphic variants not supported")
    | Ptyp_constr ({ txt = Lident "t"; _ }, []) ->
      (* In the case of `type t = prefix * string ;; type relative_path = t`, we
         have already defined a RelativePath type because we renamed t in the
         first declaration to the name of the module. We can just skip the second
         declaration introducing the alias. *)
      let mod_name_as_type = convert_type_name (curr_module_name ()) in
      if String.equal name mod_name_as_type then
        raise
          (Skip_type_decl
             ("it is an alias to type t, which was already renamed to "
             ^ mod_name_as_type))
      else
        sprintf
          "%s%spub type %s = %s;"
          doc
          attr
          (rust_type name lifetime tparams |> rust_type_to_string)
          mod_name_as_type
    | Ptyp_constr ({ txt = id; _ }, targs) ->
      let id = longident_to_string id in
      let ty_name = id |> String.split ~on:':' |> List.last_exn in
      if
        List.length td.ptype_params = List.length targs
        && String.(self () = ty_name)
        && not mutual_rec
      then (
        add_ty_reexport id;
        raise (Skip_type_decl ("it is a re-export of " ^ id))
      ) else
        let ty = Convert_type.core_type ~safe_ints ty in
        if should_add_rcoc name then
          sprintf
            "%s%spub type %s = std::sync::Arc<%s>;"
            doc
            attr
            (rust_type name lifetime tparams |> rust_type_to_string)
            (rust_type_to_string ty)
        else if should_be_newtype name then
          sprintf
            "%s struct %s (%s pub %s);%s\n%s"
            (attrs_and_vis Not_an_enum ~force_derive_copy:false)
            (rust_type name lifetime tparams |> rust_type_to_string)
            (rust_de_field_attr [ty] td.ptype_attributes)
            (rust_type_to_string ty)
            (implements ~force_derive_copy:false)
            (deserialize_in_arena_macro ~force_derive_copy:false)
        else
          sprintf
            "%s%spub type %s = %s;"
            doc
            attr
            (rust_type name lifetime tparams |> rust_type_to_string)
            (deref ty |> rust_type_to_string)
    | _ ->
      if should_use_alias_instead_of_tuple_struct name then
        let ty =
          Convert_type.core_type ~safe_ints ty |> deref |> rust_type_to_string
        in
        sprintf
          "%s%spub type %s = %s;"
          doc
          attr
          (rust_type name lifetime tparams |> rust_type_to_string)
          ty
      else
        let ty =
          match ty.ptyp_desc with
          | Ptyp_tuple tys ->
            map_and_concat
              ~f:(fun ty ->
                Convert_type.core_type ~safe_ints ty |> fun t ->
                sprintf
                  "%s pub %s"
                  (rust_de_field_attr [t] td.ptype_attributes)
                  (rust_type_to_string t))
              ~sep:","
              tys
            |> sprintf "(%s)"
          | _ ->
            Convert_type.core_type ~safe_ints ty
            |> rust_type_to_string
            |> sprintf "(pub %s)"
        in
        sprintf
          "%s struct %s %s;%s\n%s"
          (attrs_and_vis Not_an_enum ~force_derive_copy:false)
          (rust_type name lifetime tparams |> rust_type_to_string)
          ty
          (implements ~force_derive_copy:false)
          (deserialize_in_arena_macro ~force_derive_copy:false))
  (* Variant types, including GADTs. *)
  | (Ptype_variant ctors, None) ->
    (* Filter out GADT constructors whose return type references an excluded type *)
    let ctors =
      List.filter ctors ~f:(fun cd -> not (pcd_res_references_excluded_type cd))
    in
    let all_nullary =
      List.for_all ctors ~f:(fun c -> 0 = ctor_arg_len c.pcd_args)
    in
    let lifetime =
      if all_nullary then
        (* prevent generating unused lifetime parameters *)
        []
      else
        lifetime
    in
    let force_derive_copy = all_nullary in
    let box_fields = should_box_variant name in
    let num_variants = List.length ctors in
    let enum_kind =
      if not all_nullary then
        Sum_type { num_variants }
      else
        let max_value =
          ctors
          |> List.filter_map ~f:variant_constructor_value
          |> List.map ~f:int_of_string
          |> List.fold ~init:0 ~f:max
        in
        C_like { max_value; num_variants }
    in
    let ctors =
      map_and_concat
        ctors
        ~f:
          (variant_constructor_declaration
             ~safe_ints
             ~box_fields
             ~original_type_name)
    in
    sprintf
      "%s enum %s {\n%s}%s\n%s"
      (attrs_and_vis enum_kind ~force_derive_copy)
      (rust_type name lifetime tparams |> rust_type_to_string)
      ctors
      (implements ~force_derive_copy)
      (deserialize_in_arena_macro ~force_derive_copy)
  (* Record types. *)
  | (Ptype_record labels, None) ->
    let prefix = find_record_label_prefix labels in
    let labels =
      declare_record_arguments
        ~safe_ints
        labels
        ~pub:true
        ~prefix
        ~original_type_name
    in
    sprintf
      "%s struct %s %s%s\n%s"
      (attrs_and_vis
         Not_an_enum
         ~force_derive_copy:false
         ~additional_attrs:(record_prefix_attr prefix))
      (rust_type name lifetime tparams |> rust_type_to_string)
      labels
      (implements ~force_derive_copy:false)
      (deserialize_in_arena_macro ~force_derive_copy:false)
  (* `type foo`; an abstract type with no specified implementation. This doesn't
     mean much outside of an .mli, I don't think. *)
  | (Ptype_abstract, None) ->
    raise (Skip_type_decl "Abstract types without manifest not supported")
  (* type foo += A, e.g. the exn type. *)
  | (Ptype_open, None) -> raise (Skip_type_decl "Open types not supported")

let type_declaration ?(mutual_rec = false) td =
  (* We keep the original so we can look up comments in the .mli *)
  let original_type_name = td.ptype_name.txt in
  let rust_name =
    if String.equal original_type_name "t" then
      curr_module_name ()
    else
      original_type_name
  in
  let name = convert_type_name rust_name in
  let name = rename name in
  let mod_name = curr_module_name () in
  if has_oxidize_exclude_attr td.ptype_attributes then begin
    log "Not converting type %s::%s: marked [@@oxidize.exclude]" mod_name name;
    add_excluded_type original_type_name
  end else if denylisted name then
    log "Not converting type %s::%s: it was denylisted" mod_name name
  else
    match Configuration.extern_type name with
    | Some extern_type ->
      log "Not converting type %s::%s: re-exporting instead" mod_name name;
      add_decl name (sprintf "pub use %s;" extern_type)
    | None ->
      (try
         let safe_ints = Configuration.safe_ints ~mod_name ~name in
         with_self name (fun () ->
             add_decl
               name
               (type_declaration
                  ~mutual_rec
                  ~safe_ints
                  ~original_type_name
                  name
                  td))
       with
      | Skip_type_decl reason ->
        log "Not converting type %s::%s: %s" mod_name name reason)
