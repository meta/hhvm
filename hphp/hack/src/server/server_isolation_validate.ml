(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(* [Package_info.add_package] raises on a name already configured, so this has
   to be one no repository would use. *)
let candidate_package_name = "hh_isolation_candidate"

(** The candidate as a package the typechecker will enforce.

    Membership is a string prefix test, so listing files individually assigns
    exactly this set — and also claims a path that merely extends a candidate
    path, [a/B.php.expect] beside [a/B.php].

    [is_implicit] must stay false, or resolution treats the entry as a family
    and answers with a synthesized member instead. *)
let candidate_package (files : Relative_path.t list) : Package.t =
  {
    Package.name = (Pos.none, candidate_package_name);
    Package.includes = [];
    Package.soft_includes = [];
    Package.include_paths =
      List.map files ~f:(fun f -> (Pos.none, Relative_path.suffix f));
    Package.enable_strict_isolation = true;
    (* Contradicts the strict isolation the candidate opts into. *)
    Package.allow_deployed_packages_checking = false;
    Package.is_implicit = false;
  }

(** [ctx] with the candidate added to the packages already configured.

    Both the parser and the typechecker options carry a copy and both must be
    updated: a candidate in only one resolves to nothing, which reads as a
    symbol in no package — accessible from anywhere. *)
let with_candidate_package
    (ctx : Provider_context.t) (files : Relative_path.t list) :
    Provider_context.t =
  let popt = Provider_context.get_popt ctx in
  let package_info =
    Package_info.add_package
      popt.Parser_options.package_info
      (candidate_package files)
  in
  Provider_context.empty_for_tool
    ~popt:{ popt with Parser_options.package_info }
    ~tcopt:
      (Typechecker_options.set_package_info
         (Provider_context.get_tcopt ctx)
         package_info)
    ~backend:(Provider_context.get_backend ctx)
    ~deps_mode:(Provider_context.get_deps_mode ctx)

(** Every file the dependency graph says depends on a symbol [file] defines. *)
let dependents_of_file ctx deps_mode naming_table file : Relative_path.Set.t =
  match Naming_table.get_file_info naming_table file with
  | None -> Relative_path.Set.empty
  | Some file_info ->
    let symbols = Typing_deps.deps_of_file_info file_info in
    List.fold symbols ~init:Relative_path.Set.empty ~f:(fun acc symbol ->
        let dependent_symbols =
          Typing_deps.get_ideps_from_hash deps_mode symbol
        in
        let dependent_files = Naming_provider.get_files ctx dependent_symbols in
        Relative_path.Set.union acc dependent_files)

(** Files outside [files] that could hold a reference into it, per the
    dependency graph.

    Trusting the graph is the premise the cluster itself rests on, so this
    cannot catch a reference the graph does not record. The independent
    alternative costs a full check per candidate. *)
let referrers ctx naming_table (files : Relative_path.t list) :
    Relative_path.Set.t =
  let deps_mode = Provider_context.get_deps_mode ctx in
  let candidate = Relative_path.Set.of_list files in
  let dependents =
    List.fold files ~init:Relative_path.Set.empty ~f:(fun acc file ->
        Relative_path.Set.union
          acc
          (dependents_of_file ctx deps_mode naming_table file))
  in
  Relative_path.Set.diff dependents candidate

(** Whether [error] is a reason the candidate could not be this package.

    Any error that refers to the candidate counts, whatever its code:
    enumerating the package error codes instead would answer "isolatable" for
    any way of breaking a package we had not thought to list.

    Warnings do not count. The repository typechecks clean, which is what lets
    an error be attributed to the synthesized package — but that holds for the
    errors the repository is configured to see, and the filter used here turns
    on every warning besides. Those fire on code that was already there:
    "this null coalesce will always evaluate to its left-hand side" is a fair
    remark about a candidate's file and no reason it cannot be a package.

    Referring to the candidate means some reason inside it: a diagnostic's
    reasons sit at what is referred to, so this is what distinguishes a
    reference into the candidate from a checked file's own unrelated complaint.
    The claim may be inside or outside — outside is the reference in that a
    cluster must not have, and inside is the candidate reaching into its own
    [__tests__], which strict isolation no longer exempts. Both disqualify it. *)
let is_violation ~(candidate : Relative_path.Set.t) error : bool =
  match error.User_diagnostic.severity with
  | User_diagnostic.Warning _ -> false
  | User_diagnostic.Err ->
    List.exists (User_diagnostic.reason_messages error) ~f:(fun reason ->
        Relative_path.Set.mem
          candidate
          (Pos_or_decl.filename (Message.get_message_pos reason)))

let violation_of_error error :
    Server_command_types.Isolation_validation.violation =
  let pos = User_diagnostic.get_pos error in
  let message =
    Message.get_message_str (User_diagnostic.claim_message error)
    (* The package the check synthesizes has a name only it knows. *)
    |> String.substr_replace_all
         ~pattern:(Printf.sprintf "package `%s`" candidate_package_name)
         ~with_:"the candidate package"
  in
  Server_command_types.Isolation_validation.
    {
      referrer = Relative_path.suffix (Pos.filename pos);
      line = fst (Pos.line_column pos);
      message;
    }

(** Typecheck [path] under [ctx] and keep the errors that say [candidate] could
    not be a package. *)
let violations_in ctx ~candidate path :
    Server_command_types.Isolation_validation.violation list =
  let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
  let { Tast_provider.Compute_tast_and_errors.diagnostics; _ } =
    Tast_provider.compute_tast_and_errors_unquarantined
      ~ctx
      ~entry
      ~error_filter:Tast_provider.ErrorFilter.default
  in
  Diagnostics.get_diagnostic_list diagnostics
  |> List.filter ~f:(is_violation ~candidate)
  |> List.map ~f:violation_of_error

(** Paths the candidate cannot actually claim, because the file names a package
    of its own with [<<file: __PackageOverride(...)>>].

    An override wins over include-path matching, so the synthesized package
    never takes the file and the answer would be about a candidate missing it.
    Reported like an unknown path rather than worked around: the cluster asked
    about is not one this command can construct.

    The override-aware resolver is documented as too slow for the typechecker;
    it is used here over the candidate's own files only, which we are about to
    read and typecheck anyway. *)
let overridden_files ctx (files : Relative_path.t list) : string list =
  List.filter_map files ~f:(fun file ->
      match File_provider.get_contents file with
      | None -> None
      | Some content ->
        let (_, is_overridden) =
          Package_provider.get_package_with_override_for_file_no_env
            ctx
            ~path:(Relative_path.suffix file)
            ~content
        in
        if is_overridden then
          Some (Relative_path.suffix file)
        else
          None)

let go
    (genv : Server_env.genv)
    (env : Server_env.env)
    ~(files : Relative_path.t list) :
    Server_command_types.Isolation_validation.result =
  let ctx = Provider_utils.ctx_from_server_env env in
  let allow_repackaging =
    genv.Server_env.local_config
      .Server_local_config.isolation_allow_decl_repackaging
  in
  (* Two ways this server cannot answer, both reported at once since each takes
     a restart to fix. Raising would take the server down and reach the caller
     as a bare disconnection, so the reason is returned instead. *)
  let uses_rust_backend =
    match Provider_context.get_backend ctx with
    (* The Rust backend copies the package configuration when it initialises,
       so a candidate added to [ctx] never reaches the code that enforces
       packages: every file stays where it was and the run reports violations
       that are not there. *)
    | Provider_backend.Rust_provider_backend _ -> true
    | _ -> false
  in
  let refused =
    let problems =
      List.filter_map
        [
          ( not allow_repackaging,
            "this server does not permit decl repackaging",
            "isolation_allow_decl_repackaging=true" );
          ( uses_rust_backend,
            "the Rust provider backend keeps its own copy of the package configuration, which a synthesized package cannot reach",
            "rust_provider_backend=false" );
        ]
        ~f:(fun (applies, reason, setting) ->
          Option.some_if applies (reason, setting))
    in
    if List.is_empty problems then
      None
    else
      Some
        (Printf.sprintf
           "%s; restart the server with %s. Do this on a server nothing else uses: the check rebuilds declarations in the server's shared memory."
           (String.concat ~sep:", and " (List.map problems ~f:fst))
           (String.concat ~sep:" and " (List.map problems ~f:snd)))
  in
  match refused with
  | Some _ ->
    Server_command_types.Isolation_validation.
      {
        refused;
        isolatable = false;
        files_checked = 0;
        violations = [];
        unknown_files = [];
        overridden_files = [];
      }
  | None ->
    let naming_table = env.Server_env.naming_table in
    let candidate = Relative_path.Set.of_list files in
    let (known, unknown) =
      List.partition_tf files ~f:(fun file ->
          Option.is_some (Naming_table.get_file_info naming_table file))
    in
    let unknown_files = List.map unknown ~f:Relative_path.suffix in
    let overridden_files = overridden_files ctx known in
    (* One source per kind of violation. *)
    let to_check =
      Relative_path.Set.union
        (* A reference into the candidate is reported at the referring file. *)
        (referrers ctx naming_table files)
        (* The excluded-path violation is reported inside the candidate — a
           package using its own [__tests__] — so it surfaces only by checking
           the candidate's own files. [known] alone: an unknown path has nothing
           to typecheck, and is answered through [unknown_files]. *)
        (Relative_path.Set.of_list known)
    in
    let ctx = with_candidate_package ctx files in
    let violations =
      Server_isolation_decls.with_repackaged_decls
        ctx
        naming_table
        files
        ~allow_repackaging
        ~f:(fun () ->
          Relative_path.Set.elements to_check
          |> List.concat_map ~f:(violations_in ctx ~candidate))
    in
    Server_command_types.Isolation_validation.
      {
        refused = None;
        (* A candidate we could not construct in full is not reported isolatable.
           A path we had to set aside contributes no referrers, so answering from
           the rest would answer about a smaller candidate than the one asked
           about. *)
        isolatable =
          List.is_empty violations
          && List.is_empty unknown_files
          && List.is_empty overridden_files;
        files_checked = Relative_path.Set.cardinal to_check;
        violations;
        unknown_files;
        overridden_files;
      }
