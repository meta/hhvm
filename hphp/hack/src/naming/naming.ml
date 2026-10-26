(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Module "naming" a program.
 *
 * The naming phase consists in several things
 * 1- get all the global names
 * 2- transform all the local names into a unique identifier
 *)

open Hh_prelude
open Common
module Env = Naming_phase_env

(*****************************************************************************)
(* Helpers *)
(*****************************************************************************)
let (on_error, reset_errors, get_errors) =
  let naming_errs = ref Naming_phase_error.empty in
  let reset_errors () = naming_errs := Naming_phase_error.empty
  and get_errors () = !naming_errs
  and on_error err = naming_errs := Naming_phase_error.add !naming_errs err in
  (on_error, reset_errors, get_errors)

let invalid_expr_ = Naming_phase_error.invalid_expr_

let filename_in_allowed filename allowed =
  List.exists
    ~f:(fun allowed_file ->
      let len = String.length allowed_file in
      if len > 0 then
        match allowed_file.[len - 1] with
        | '*' ->
          let allowed_dir = String.sub allowed_file ~pos:0 ~len:(len - 1) in
          String.is_prefix filename ~prefix:allowed_dir
        | _ -> String.equal allowed_file filename
      else
        false)
    allowed

let mk_env filename tcopt =
  let file_str = Relative_path.suffix filename in
  let is_hhi = String.is_suffix (Relative_path.suffix filename) ~suffix:".hhi"
  and is_systemlib = Typechecker_options.is_systemlib tcopt
  and allow_module_def =
    Typechecker_options.allow_all_files_for_module_declarations tcopt
    || filename_in_allowed
         file_str
         (Typechecker_options.allowed_files_for_module_declarations tcopt)
  and allow_ignore_readonly =
    filename_in_allowed
      file_str
      (Typechecker_options.allowed_files_for_ignore_readonly tcopt)
  and everything_sdt = Typechecker_options.everything_sdt tcopt
  and supportdynamic_type_hint_enabled =
    Typechecker_options.legacy_experimental_feature_enabled
      tcopt
      Typechecker_options.experimental_supportdynamic_type_hint
  and soft_as_like =
    Typechecker_options.interpret_soft_types_as_like_types tcopt
  and consistent_ctor_level =
    Typechecker_options.explicit_consistent_constructors tcopt
  and typed_open_shapes = Typechecker_options.typed_open_shapes tcopt
  and named_variadic_type = Typechecker_options.named_variadic_type tcopt
  and variadic_named_parameters =
    Typechecker_options.variadic_named_parameters tcopt
  in
  Env.
    {
      empty with
      is_hhi;
      is_systemlib;
      consistent_ctor_level;
      allow_module_def;
      everything_sdt;
      supportdynamic_type_hint_enabled;
      soft_as_like;
      allow_ignore_readonly;
      typed_open_shapes;
      named_variadic_type;
      variadic_named_parameters;
    }

let passes =
  [
    (* Stop on `Invalid` expressions *)
    Naming_guard_invalid.pass;
    (* Canonicalization passes -------------------------------------------- *)
    (* Remove top-level file attributes, noop and markup statements *)
    Naming_elab_defs.pass;
    (* Remove function bodies when in hhi mode *)
    Naming_elab_func_body.pass;
    (* Flatten `Block` statements *)
    Naming_elab_block.pass;
    (* Strip `Hsoft` hints or replace with `Hlike` *)
    Naming_elab_soft.pass;
    (* Elaborate `Happly` to canonical representation, if any *)
    Naming_elab_happly_hint.pass on_error;
    (* Elaborate class identifier expressions (`CIexpr`) to canonical
        representation: `CIparent`, `CIself`, `CIstatic`, `CI` _or_
       `CIexpr (_,_, Lvar _ | This )` *)
    Naming_elab_class_id.pass on_error;
    (* Elaborate `Collection` to `ValCollection` or `KeyValCollection` *)
    Naming_elab_collection.pass on_error;
    (* Check that user attributes are well-formed *)
    Naming_elab_user_attributes.pass on_error;
    (* Replace import expressions with invalid expression marker *)
    Naming_elab_import.pass;
    (* Elaborate local variables to canonical representation *)
    Naming_elab_lvar.pass;
    (* Warn of explicit use of builtin enum classes; make subtyping of
       enum classes explicit*)
    Naming_elab_enum_class.pass on_error;
    (* Elaborate class members & xhp attributes  *)
    Naming_elab_class_members.pass on_error;
    (* Elaborate special function calls to canonical representation, if any *)
    Naming_elab_call.top_down_pass;
    Naming_elab_call.bottom_up_pass on_error;
    (* Elaborate invariant calls to canonical representation *)
    Naming_elab_invariant.pass on_error;
    (* -- Mark invalid hints and expressions & miscellaneous validation --- *)
    Naming_elab_retonly_hint.pass on_error;
    Naming_elab_wildcard_hint.pass on_error;
    (* Replace uses to `self` in shape field names with referenced class *)
    Naming_elab_shape_field_name.top_down_pass;
    Naming_elab_shape_field_name.bottom_up_pass on_error;
    Naming_elab_this_hint.pass on_error;
    Naming_elab_haccess_hint.pass on_error;
    (* Validate / replace invalid uses of dynamic classes in `New` and `Class_get`
       expressions *)
    Naming_elab_dynamic_class_name.pass on_error;
    (* Replace non-constant class or global constant with invalid expression marker *)
    Naming_elab_const_expr.top_down_pass on_error;
    Naming_elab_const_expr.bottom_up_pass on_error;
    (* Replace malformed key / value bindings in as expressions with invalid
       local var markers *)
    Naming_elab_as_expr.pass on_error;
    (* Validate hints used in `Cast` expressions *)
    Naming_validate_cast_expr.pass on_error;
    (* Validate where `dynamic` can be used in a hint *)
    Naming_validate_dynamic_hint.pass on_error;
    (* Check for duplicate function parameter names *)
    Naming_validate_fun_params.pass on_error;
    (* Validate use of `require implements`, `require extends` and
       `require class` declarations for traits, interfaces and classes *)
    Naming_validate_class_req.pass on_error;
    (* Validation dealing with common xhp naming errors *)
    Naming_validate_xhp_name.pass on_error;
    (* Validate polymorphic lambda expressions, ensuring return hint and all
       parameter hints are provided *)
    Naming_validate_polymorphic_lambda_expr.pass on_error;
    (* -- Elaboration & validation under typechecker options -------------- *)
    (* Add `supportdyn` and `Like` wrappers everywhere - under `everything-sdt`
       typechecker option *)
    Naming_elab_everything_sdt.top_down_pass;
    Naming_elab_everything_sdt.bottom_up_pass;
    (* Validate constructors under
       `consistent-explicit_consistent_constructors` typechecker option *)
    Naming_validate_consistent_construct.pass on_error;
    (* Validate  use of `SupportDyn` class - depends on `enable-supportdyn`
       and `everything_sdt` typechecker options *)
    Naming_validate_supportdyn.pass on_error;
    (* Validate use of module definitions - depends on:
        - `allow_all_files_for_module_declarations`
        - `allowed_files_for_module_declarations`
       typechecker options *)
    Naming_validate_module.pass on_error;
    (* Validate use of `optional` in function type hints *)
    Naming_validate_function_hint_optional_parameters.pass on_error;
    (* Reject `shape(..., T...)` typed open shapes unless tco_typed_open_shapes
       is enabled *)
    Naming_validate_typed_open_shape.pass on_error;
    (* Reject named variadic parameters in function type hints unless
       tco_named_variadic_type is enabled *)
    Naming_validate_named_variadic_type.pass on_error;
    (* Reject variadic named parameters on function definitions (e.g.
       `named int...`) unless tco_variadic_named_parameters is enabled.
       Note: the variadic cannot be named. We have no immediate plans to support `named int ...$xs`.
    *)
    Naming_validate_variadic_named_parameter.pass on_error;
    (* Validate that return type of __clone is void *)
    Naming_validate_clone_return_hint.pass on_error;
  ]

(* If we don't delegate to Rust, then all naming passes are done here in OCaml.
   *)
let ( elab_core_program,
      elab_core_class,
      elab_core_fun_def,
      elab_core_module_def,
      elab_core_gconst,
      elab_core_typedef ) =
  Naming_phase_pass.mk_visitor passes

let elab_elem
    elem ~elab_ns ~elab_capture ~validate_await ~elab_core custom_err_config =
  reset_errors ();
  let elem = elab_ns elem |> elab_capture in
  validate_await elem;
  let elem = elab_core elem in
  Naming_phase_error.emit (get_errors ()) ~custom_err_config;
  reset_errors ();
  elem

let program_filename defs =
  let open Aast_defs in
  let rec aux = function
    | Fun fun_def :: _ -> Pos.filename fun_def.fd_fun.f_span
    | Class class_ :: _ -> Pos.filename class_.c_span
    | Stmt (pos, _) :: _ -> Pos.filename pos
    | Typedef typedef :: _ -> Pos.filename typedef.t_span
    | Constant gconst :: _ -> Pos.filename gconst.cst_span
    | Module module_def :: _ -> Pos.filename module_def.md_span
    | _ :: rest -> aux rest
    | _ -> Relative_path.default
  in
  aux defs

(**************************************************************************)
(* The entry points to CHECK the program, and transform the program *)
(**************************************************************************)

let fun_def ctx fd =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = Pos.filename fd.Aast.fd_fun.Aast.f_span in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_fun_def
  and elab_capture = Naming_captures.elab_fun_def ~custom_err_config
  and validate_await = Naming_validate_await.validate_fun_def on_error
  and elab_core = elab_core_fun_def (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    fd
    custom_err_config

let class_ ctx c =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = Pos.filename c.Aast.c_span in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_class_
  and elab_capture = Naming_captures.elab_class ~custom_err_config
  and validate_await = Naming_validate_await.validate_class on_error
  and elab_core = elab_core_class (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    c
    custom_err_config

let module_ ctx md =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = Pos.filename md.Aast.md_span in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_module_def
  and elab_capture = Naming_captures.elab_module_def ~custom_err_config
  and validate_await _ = ()
  and elab_core = elab_core_module_def (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    md
    custom_err_config

let global_const ctx cst =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = Pos.filename cst.Aast.cst_span in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_gconst
  and elab_capture = Naming_captures.elab_gconst ~custom_err_config
  and validate_await _ = ()
  and elab_core = elab_core_gconst (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    cst
    custom_err_config

let typedef ctx td =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = Pos.filename @@ td.Aast.t_span in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_typedef
  and elab_capture = Naming_captures.elab_typedef ~custom_err_config
  and validate_await _ = ()
  and elab_core = elab_core_typedef (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    td
    custom_err_config

let emit_toplevel_stmt_errors stmts ~custom_err_config =
  let agg =
    List.fold_left stmts ~init:Naming_phase_error.empty ~f:(fun acc (pos, _) ->
        let err =
          Naming_phase_error.naming (Naming_error.Toplevel_statement pos)
        in
        Naming_phase_error.add acc err)
  in
  Naming_phase_error.emit agg ~custom_err_config

let fun_def_of_stmts ctx stmts : Nast.fun_def option =
  let popt = Provider_context.get_popt ctx in
  let custom_err_config =
    let tcopt = Provider_context.get_tcopt ctx in
    Typechecker_options.custom_error_config tcopt
  in
  let stmts =
    List.filter stmts ~f:(function
        | (_, Aast.Markup _) -> false
        | _ -> true)
  in
  let () = emit_toplevel_stmt_errors stmts ~custom_err_config in
  match stmts with
  | [] -> None
  | (pos, _) :: tail ->
    let pos = tail |> List.map ~f:fst |> List.fold ~init:pos ~f:Pos.merge in
    (* Match the logic the lowerer does to assign packages to functions based on file name.
     * Note: for Hack notebooks, we use a file name that ensures we have broad access (D92944036)
     *)
    let fd_package =
      let pos_suffix = Relative_path.suffix (Pos.filename pos) in
      Package_provider.get_package_for_file ctx ~path:pos_suffix
      |> Option.map ~f:(fun pkg ->
             Aast.PackageConfigAssignment (snd pkg.Package.name))
    in
    let ns_ns_uses =
      popt.Parser_options.auto_namespace_map
      |> S_map.of_list
      |> S_map.union Namespace_env.(empty_with_default.ns_ns_uses)
    in
    Some
      Aast.
        {
          fd_name =
            (pos, "")
            (* An empty function name is appropriate, as the AST can already include empty function names due to parse errors *);
          fd_namespace =
            Namespace_env.
              {
                empty_with_default with
                ns_ns_uses;
                ns_disable_xhp_element_mangling =
                  popt.Parser_options.disable_xhp_element_mangling;
              };
          fd_file_attributes = [];
          fd_mode = File_info.Mstrict;
          fd_fun =
            {
              f_span = pos;
              f_readonly_this = None;
              f_tparams = [];
              f_annotation = ();
              f_readonly_ret = None;
              f_ret =
                ( (),
                  Some
                    ( pos,
                      Happly
                        ( (pos, Naming_special_names.Classes.cAwaitable),
                          [(pos, Hprim Tvoid)] ) ) );
              f_params = [];
              f_body = { fb_ast = stmts };
              f_ctxs = None;
              f_unsafe_ctxs = None;
              f_fun_kind = Ast_defs.FAsync (* enable top-level await *);
              f_user_attributes = [];
              f_external = false;
              f_hidden = false;
              f_doc_comment = None;
            };
          fd_internal = false;
          fd_module = None;
          fd_tparams = [];
          fd_where_constraints = [];
          fd_package;
        }

(** Return an updated [Nast.program] where top-level statements
* are wrapped in a function. This is not an elaboration pass
* because it affects the entire file and whole-file elaboration passes
* are not used in `Typing_check_job.calc_errors_and_tast`.
* Shared logic with `Typing_check_job.calc_errors_and_tast`
* is in `fun_def_of_stmts`.
*)
let adjust_toplevel_stmts ctx program : Nast.program =
  let (toplevel_defs, toplevel_stmts) =
    List.partition_map program ~f:(function
        | Aast.Stmt s -> Either.Second s
        | def -> Either.First def)
  in
  match fun_def_of_stmts ctx toplevel_stmts with
  | Some def -> Aast.Fun def :: toplevel_defs
  | None -> toplevel_defs

let program ctx program =
  let tcopt = Provider_context.get_tcopt ctx in
  let custom_err_config = Typechecker_options.custom_error_config tcopt in
  let filename = program_filename program in
  let program = adjust_toplevel_stmts ctx program in
  let elab_ns = Naming_elaborate_namespaces_endo.elaborate_program
  and elab_capture = Naming_captures.elab_program ~custom_err_config
  and validate_await = Naming_validate_await.validate_program on_error
  and elab_core = elab_core_program (mk_env filename tcopt) in
  elab_elem
    ~elab_ns
    ~elab_capture
    ~validate_await
    ~elab_core
    program
    custom_err_config
