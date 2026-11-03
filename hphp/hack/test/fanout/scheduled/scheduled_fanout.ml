(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

type files = (string * string) list

type action = Write_file of string * string

type type_check_step =
  | Check of {
      file: string;
      after_check: action list;
    }
  | Remaining

type iteration = {
  before_type_check: action list;
  disk_needs_parsing: string list;
  after_indexing: action list;
  type_check: type_check_step list;
}

let default_iteration =
  {
    before_type_check = [];
    disk_needs_parsing = [];
    after_indexing = [];
    type_check = [Remaining];
  }

type scenario = {
  initial_files: files;
  iterations: iteration list;
}

type initialization =
  | Full_check
  | Saved_state

exception Diagnostics_mismatch of string

exception Internal_typechecker_error of string

let expect_known_fanout_bug
    ~(bug : string) ~(matches : exn -> bool) (check : unit -> unit) =
  match check () with
  | () ->
    failwith
      (Printf.sprintf
         "Expected fanout bug %s did not reproduce. You probably fixed this bug; remove the wrapper and keep the correctness check."
         bug)
  | exception
      ((Diagnostics_mismatch _ | Internal_typechecker_error _) as failure) ->
    if not (matches failure) then raise failure

let matches_diagnostics expected contents =
  if String.equal contents "No diagnostics." then
    List.is_empty expected
  else
    let open Yojson.Safe.Util in
    try
      let actual =
        Yojson.Safe.seq_from_string contents
        |> Stdlib.List.of_seq
        |> List.map ~f:(fun diagnostic ->
               match member "message" diagnostic with
               | `List (claim :: _) ->
                 let path = member "path" claim |> to_string in
                 let file =
                   Option.value
                     (String.chop_prefix path ~prefix:"root|")
                     ~default:path
                 in
                 Some
                   ( file,
                     member "code" claim |> to_int,
                     member "descr" claim |> to_string )
               | _ -> None)
        |> Option.all
      in
      Option.value_map actual ~default:false ~f:(fun actual ->
          List.equal
            Poly.equal
            (List.sort expected ~compare:Poly.compare)
            (List.sort actual ~compare:Poly.compare))
    with
    | Yojson.Json_error _
    | Type_error _ ->
      false

let matches_diagnostics_mismatch ~incremental ~full = function
  | Diagnostics_mismatch message ->
    let prefix =
      "The last incremental iteration disagrees with a fresh full check.\n\n"
      ^ "Incremental diagnostics:\n"
    in
    let separator = "\n\nFull-check diagnostics:\n" in
    Option.value_map
      (String.chop_prefix message ~prefix)
      ~default:false
      ~f:(fun contents ->
        Option.value_map
          (String.substr_index contents ~pattern:separator)
          ~default:false
          ~f:(fun index ->
            matches_diagnostics incremental (String.prefix contents index)
            && matches_diagnostics
                 full
                 (String.drop_prefix contents (index + String.length separator))))
  | _ -> false

let file_path root suffix =
  if
    (not (Filename.is_relative suffix))
    || List.mem (String.split suffix ~on:'/') ".." ~equal:String.equal
  then
    invalid_arg
      (Printf.sprintf "Expected a repository-relative path: %s" suffix);
  Filename.concat root suffix

let write_files root files =
  List.iter files ~f:(fun (suffix, contents) ->
      let file = file_path root suffix in
      Sys_utils.mkdir_p (Filename.dirname file);
      Sys_utils.write_file ~file contents)

let action_files actions =
  List.map actions ~f:(function Write_file (path, contents) -> (path, contents))

let run_actions root actions = write_files root (action_files actions)

let parsing_paths root files =
  List.map files ~f:(fun suffix ->
      Relative_path.create Relative_path.Root (file_path root suffix))
  |> Relative_path.Set.of_list

let parser_options =
  Parser_options.
    {
      default with
      disable_xhp_element_mangling = false;
      deregister_php_stdlib = true;
      use_legacy_experimental_feature_config = false;
    }

let setup ~root ~hhi ~tmp =
  Unix.putenv "HH_TEST_MODE" "1";
  Printexc.record_backtrace true;
  Folly.ensure_folly_init ();
  Event_logger.init_fake ();
  Hh_logger.Level.(set_min_level Error);
  Relative_path.set_path_prefix Relative_path.Root (Path.make root);
  Relative_path.set_path_prefix Relative_path.Tmp (Path.make tmp);
  Hhi.set_hhi_root_for_unit_test (Path.make hhi);
  Server_progress.disable ();
  let shm_config =
    {
      Shared_mem.default_config with
      shm_use_sharded_hashtbl = true;
      shm_cache_size = 2 * 1024 * 1024 * 1024;
    }
  in
  let (_ : Shared_mem.handle) = Shared_mem.init ~num_workers:0 shm_config in
  Typing_deps.trace := true;
  let tcopt =
    Global_options.set
      ~po:parser_options
      ~tco_fetch_remote_old_decls:false
      ~tco_populate_member_heaps:false
      Global_options.default
  in
  Hh_server_provider_backend.make
    (Decl_fold_options.from_global_options tcopt)
    (Decl_parser_options.from_parser_options parser_options)
  |> Provider_backend.set_rust_backend;
  let config =
    Server_config.set_tc_options Server_config.default_config tcopt
    |> fun config -> Server_config.set_parser_options config parser_options
  in
  let genv =
    {
      Server_env_build.default_genv with
      Server_env.options = Server_args.default_options ~root;
      config;
      local_config =
        {
          Server_env_build.default_genv.local_config with
          use_distc = false;
          rust_provider_backend = true;
          shm_use_sharded_hashtbl = true;
          populate_member_heaps = false;
          disable_rust_provider_shallow_decl_invalidation =
            tcopt
              .Global_options
               .tco_disable_rust_provider_shallow_decl_invalidation;
          enable_annotation_agnostic_decl_diffing =
            tcopt.Global_options.tco_enable_annotation_agnostic_decl_diffing;
          prefetch_decls_enabled = false;
          produce_streaming_errors = false;
        };
    }
  in
  let env =
    Server_env_build.make_env
      ~init_id:"scheduled_fanout"
      ~deps_mode:(Typing_deps_mode.InMemoryMode None)
      config
  in
  let hhi_files =
    Hhi.get_raw_hhi_contents ()
    |> Array.to_list
    |> List.map ~f:(fun (suffix, _) ->
           Relative_path.create Relative_path.Hhi (Filename.concat hhi suffix))
    |> Relative_path.Set.of_list
  in
  (genv, { env with Server_env.disk_needs_parsing = hhi_files })

let reject_internal_errors ~check env =
  let internal_errors =
    Diagnostics.get_diagnostic_list
      ~drop_fixmed:false
      env.Server_env.diagnostics
    |> List.filter ~f:(fun diagnostic ->
           match
             Error_codes.Typing.of_enum (User_diagnostic.get_code diagnostic)
           with
           | Some
               ( Error_codes.Typing.ExceptionOccurred
               | Error_codes.Typing.InvariantViolated ) ->
             true
           | Some Error_codes.Typing.InternalError ->
             (* Code zero is also used for ordinary experimental-feature errors.
                Position validation can move the original message into reasons. *)
             let is_internal_message (_, message) =
               String.is_prefix message ~prefix:"Internal error: "
             in
             is_internal_message (User_diagnostic.claim_message diagnostic)
             || List.exists
                  (User_diagnostic.reason_messages diagnostic)
                  ~f:is_internal_message
           | _ -> false)
  in
  if not (List.is_empty internal_errors) then
    raise
      (Internal_typechecker_error
         (Printf.sprintf
            "Internal typechecker errors during %s; the scenario did not complete its checks:\n%s"
            check
            (Diagnostics.convert_errors_to_string internal_errors
            |> String.concat ~sep:"\n")))

let split_type_check_schedule steps =
  let rec split checks = function
    | [] -> (List.rev checks, false)
    | [Remaining] -> (List.rev checks, true)
    | Remaining :: _ ->
      invalid_arg "Remaining must be the final step in a type_check schedule"
    | Check { file; after_check } :: rest ->
      split ((file, after_check) :: checks) rest
  in
  split [] steps

let run_iteration genv env ~root ~check iteration =
  let { before_type_check; disk_needs_parsing; after_indexing; type_check } =
    iteration
  in
  let (scheduled_checks, check_remaining) =
    split_type_check_schedule type_check
  in
  let scheduled_checks =
    List.map scheduled_checks ~f:(fun (file, after_check) ->
        ( Relative_path.create Relative_path.Root (file_path root file),
          after_check ))
  in
  let scheduled_paths =
    List.fold
      scheduled_checks
      ~init:Relative_path.Set.empty
      ~f:(fun paths (path, _) ->
        if Relative_path.Set.mem paths path then
          invalid_arg
            ("Duplicate scheduled file check: " ^ Relative_path.suffix path);
        Relative_path.Set.add paths path)
  in
  run_actions root before_type_check;
  let pending_checks = ref scheduled_checks in
  let type_check_testing_hooks =
    if
      List.is_empty after_indexing
      && List.is_empty scheduled_checks
      && check_remaining
    then
      None
    else
      Some
        Server_env.
          {
            after_indexing = (fun () -> run_actions root after_indexing);
            before_file_checks =
              (fun paths ->
                let selected = Relative_path.Set.of_list paths in
                List.iter scheduled_checks ~f:(fun (path, _) ->
                    if not (Relative_path.Set.mem selected path) then
                      failwith
                        ("Scheduled file is absent from the fanout: "
                        ^ Relative_path.suffix path));
                let remaining =
                  List.filter paths ~f:(fun path ->
                      not (Relative_path.Set.mem scheduled_paths path))
                in
                if (not check_remaining) && not (List.is_empty remaining) then
                  failwith
                    "type_check omits selected files; add Remaining to the schedule";
                let checks =
                  if check_remaining then
                    scheduled_checks
                    @ List.map remaining ~f:(fun path -> (path, []))
                  else
                    scheduled_checks
                in
                let expected_paths = List.map checks ~f:fst in
                (if not (List.equal Relative_path.equal paths expected_paths)
                then
                  let names paths =
                    List.map paths ~f:Relative_path.suffix
                    |> String.concat ~sep:"; "
                  in
                  failwith
                    (Printf.sprintf
                       "Expected file checks [%s], got [%s]"
                       (names expected_paths)
                       (names paths)));
                if not (List.is_empty scheduled_checks) then
                  pending_checks := checks);
            after_file_check =
              (fun path ->
                match !pending_checks with
                | [] -> ()
                | (expected, actions) :: remaining ->
                  if not (Relative_path.equal path expected) then
                    failwith
                      (Printf.sprintf
                         "Expected %s to finish checking before %s"
                         (Relative_path.suffix expected)
                         (Relative_path.suffix path));
                  run_actions root actions;
                  pending_checks := remaining);
          }
  in
  let env =
    {
      env with
      Server_env.disk_needs_parsing =
        Relative_path.Set.union
          env.Server_env.disk_needs_parsing
          (parsing_paths root disk_needs_parsing);
      full_check_status = Server_env.Full_check_started;
      type_check_testing_hooks;
    }
  in
  let (env, (_ : Server_type_check.CheckStats.t), (_ : Telemetry.t)) =
    Cgroup_profiler.step_group "scheduled_fanout" ~log:false
    @@ Server_type_check.type_check genv env (Unix.gettimeofday ())
  in
  reject_internal_errors ~check env;
  if not (List.is_empty !pending_checks) then
    failwith "Typechecking returned before all scheduled file checks completed";
  { env with Server_env.type_check_testing_hooks = None }

let diagnostics env =
  Diagnostics.get_diagnostic_list env.Server_env.diagnostics
  |> List.map ~f:(fun diagnostic ->
         User_diagnostic.to_relative diagnostic
         |> User_diagnostic.to_json
              ~human_formatter:None
              ~filename_to_string:Relative_path.storage_to_string
         |> Yojson.Safe.to_string)
  |> List.sort ~compare:String.compare

type child_check =
  | Incremental of scenario
  | Fresh_check of string list

type child_request = {
  root: string;
  hhi: string;
  tmp: string;
  saved_state: Scheduled_fanout_saved_state.t option;
  check: child_check;
}

let run_child ({ root; hhi; tmp; saved_state; check } : child_request) =
  let (genv, env) = setup ~root ~hhi ~tmp in
  let env =
    match check with
    | Incremental { initial_files; iterations } ->
      let env =
        match saved_state with
        | Some saved_state -> Scheduled_fanout_saved_state.load saved_state env
        | None ->
          run_iteration
            genv
            env
            ~root
            ~check:"baseline"
            {
              default_iteration with
              disk_needs_parsing = List.map initial_files ~f:fst;
            }
      in
      List.foldi iterations ~init:env ~f:(fun index env iteration ->
          run_iteration
            genv
            env
            ~root
            ~check:(Printf.sprintf "iteration %d" (index + 1))
            iteration)
    | Fresh_check files ->
      run_iteration
        genv
        env
        ~root
        ~check:"fresh full check"
        { default_iteration with disk_needs_parsing = files }
  in
  diagnostics env

let child_entry :
    (child_request, unit, (string list, string) result) Daemon.entry =
  Daemon.register_entry_point "scheduled_fanout" (fun request (_, output) ->
      let result =
        try Ok (run_child request) with
        | Internal_typechecker_error message -> Error message
      in
      Daemon.to_channel output result)

(** Runs a check in an isolated child process and returns its diagnostics.
    Propagates internal typechecker errors and child process failures. *)
let in_process ~name request =
  (* Daemon restarts this test binary at child_entry with the serialized
     request. The child initializes its own provider caches and shared memory. *)
  let handle =
    Daemon.spawn
      ~channel_mode:`socket
      ~name
      (Daemon.null_fd (), Unix.stdout, Unix.stderr)
      child_entry
      request
  in
  let reaped = ref false in
  Exn.protect
    ~f:(fun () ->
      (* Daemon.from_channel deserializes the result synchronously. Read before
         waitpid: a child blocked by a full socket buffer cannot finish sending
         its result and exit until we read from the socket. *)
      let result =
        try Some (Daemon.from_channel (fst handle.Daemon.channels)) with
        | End_of_file -> None
      in
      (* Receiving a result does not establish that the child exited normally. *)
      let (_, status) = Sys_utils.waitpid_non_intr [] handle.Daemon.pid in
      reaped := true;
      match (status, result) with
      | (Unix.WEXITED 0, Some (Ok result)) -> result
      | (Unix.WEXITED 0, Some (Error message)) ->
        (* Internal typechecker failures are returned as data. Re-raise them
           for the caller even though the daemon exited successfully. *)
        raise (Internal_typechecker_error message)
      | (Unix.WEXITED code, _) ->
        failwith (Printf.sprintf "%s child exited with code %d" name code)
      | (Unix.WSIGNALED signal, _)
      | (Unix.WSTOPPED signal, _) ->
        failwith (Printf.sprintf "%s child received signal %d" name signal))
    ~finally:(fun () ->
      if !reaped then
        Daemon.close handle
      else (
        (* A failure during reading or waiting can leave the child running.
           Stop and reap it so a failing test leaves no background process. *)
        Daemon.force_quit handle;
        let (_ : int * Unix.process_status) =
          Sys_utils.waitpid_non_intr [] handle.Daemon.pid
        in
        ()
      ))

let run
    ?(initialization = Full_check) ({ initial_files; iterations } as scenario) =
  if List.is_empty iterations then
    invalid_arg "A scheduled fanout scenario must contain an iteration";
  List.iter iterations ~f:(fun iteration ->
      ignore (split_type_check_schedule iteration.type_check));
  Tempfile.with_real_tempdir @@ fun workspace ->
  let directory name =
    let path = Path.concat workspace name |> Path.to_string in
    Sys_utils.mkdir_p path;
    path
  in
  let root = directory "repo" in
  let hhi = directory "hhi" in
  let tmp = directory "tmp" in
  write_files root initial_files;
  let saved_state =
    match initialization with
    | Full_check -> None
    | Saved_state ->
      Some
        (Scheduled_fanout_saved_state.create
           ~root
           ~parser_options
           ~directory:(directory "saved_state"))
  in
  let incremental =
    in_process
      ~name:"incremental"
      { root; hhi; tmp; saved_state; check = Incremental scenario }
  in
  (* Writes are the only supported disk actions, so these are exactly the files
     in the final repository. Read their actual contents in the fresh child. *)
  let final_files =
    initial_files
    @ List.concat_map
        iterations
        ~f:(fun { before_type_check; after_indexing; type_check; _ } ->
          before_type_check
          @ after_indexing
          @ List.concat_map type_check ~f:(function
                | Check { after_check; _ } -> after_check
                | Remaining -> [])
          |> action_files)
    |> List.map ~f:fst
    |> List.dedup_and_sort ~compare:String.compare
  in
  let full =
    in_process
      ~name:"full check"
      { root; hhi; tmp; saved_state; check = Fresh_check final_files }
  in
  if not (List.equal String.equal incremental full) then
    let format diagnostics =
      if List.is_empty diagnostics then
        "No diagnostics."
      else
        List.map diagnostics ~f:(fun diagnostic ->
            Yojson.Safe.from_string diagnostic |> Yojson.Safe.pretty_to_string)
        |> String.concat ~sep:"\n"
    in
    raise
      (Diagnostics_mismatch
         (Printf.sprintf
            "The last incremental iteration disagrees with a fresh full check.\n\nIncremental diagnostics:\n%s\n\nFull-check diagnostics:\n%s"
            (format incremental)
            (format full)))

let run_expect_bug ?initialization ~bug ~incremental ~full scenario =
  expect_known_fanout_bug
    ~bug
    ~matches:(matches_diagnostics_mismatch ~incremental ~full)
    (fun () -> run ?initialization scenario)
