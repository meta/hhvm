(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 * *)

open Hh_prelude
open Utils

(** Module for us to interface with Watchman, a file watching service.
    https://facebook.github.io/watchman/ *)

(** Stuff shared between Actual and Mocked implementations. *)
module Testing_common = struct
  open Watchman_sig.Types

  let test_settings =
    {
      subscribe_mode = Some Defer_changes;
      init_timeout = Watchman_sig.Types.No_timeout;
      expression_terms = [];
      debug_logging = false;
      roots = [Path.dummy_path];
      sockname = None;
      subscription_prefix = "dummy_prefix";
    }
end

module J = Hh_json_helpers.AdhocJsonHelpers
module T = Watchman_sig.Types

module Helpers : sig
  val timeout_to_secs : T.timeout -> float option

  val max_array_elt_in_json_logging : int

  (** Verifies that a watchman response is valid JSON and free from common errors *)
  val sanitize_watchman_response :
    debug_logging:bool -> T.clock -> Yojson.Safe.t
end = struct
  let timeout_to_secs = function
    | T.No_timeout -> None
    | T.Default_timeout -> Some 1200.
    | T.Explicit_timeout timeout -> Some timeout

  (** Look for common errors in watchman responses *)
  let assert_no_error obj =
    (try
       let warning = J.get_string_val "warning" obj in
       Event_logger.watchman_warning warning;
       Hh_logger.log "Watchman warning: %s\n" warning
     with
    | J.Not_found -> ());
    (try
       let error = J.get_string_val "error" obj in
       Event_logger.watchman_error error;
       raise @@ T.Watchman_error error
     with
    | J.Not_found -> ());
    try
      let canceled = J.get_bool_val "canceled" obj in
      if canceled then (
        Event_logger.watchman_error "Subscription canceled by watchman";
        raise @@ T.Subscription_canceled_by_watchman
      ) else
        ()
    with
    | J.Not_found -> ()

  let max_array_elt_in_json_logging = 5

  (** Verifies that a watchman response is valid JSON and free from common errors *)
  let sanitize_watchman_response ~debug_logging output =
    if debug_logging then Hh_logger.info "Watchman response: %s" output;
    let response =
      try
        let response = Yojson.Safe.from_string output in
        (if not debug_logging then
          let has_changed = ref false in
          (* if debug_logging, we've already logged the full watchman response, so skip
           * logging truncated one. *)
          let json =
            Hh_json_helpers.json_truncate
              ~max_array_elt_count:max_array_elt_in_json_logging
              ~has_changed
              response
          in
          Hh_logger.info
            "Watchman response: %s%s"
            (if !has_changed then
              Printf.sprintf "(truncated) "
            else
              "")
            (* unsorted: watchman query, consumed by external tool *)
            (Yojson.Safe.to_string json));
        response
      with
      | exn ->
        let e = Exception.wrap exn in
        Hh_logger.error
          "Failed to parse string as JSON: %s\n%s"
          output
          (Exception.to_string e);
        Exception.reraise e
    in
    assert_no_error response;
    response
end

module Watchman_conn : sig
  type conn

  exception Read_payload_too_long

  val open_connection :
    timeout:Watchman_sig.Types.timeout -> sockname:string option -> conn

  (** Sends a request to watchman and returns the response. If we don't have a connection,
      a new connection will be created before the request and destroyed after the response *)
  val request :
    debug_logging:bool ->
    ?conn:conn ->
    ?timeout:Watchman_sig.Types.timeout ->
    sockname:string option ->
    Yojson.Safe.t ->
    Yojson.Safe.t

  val send_request_and_do_not_wait_for_response :
    debug_logging:bool -> conn:conn -> Yojson.Safe.t -> unit

  val blocking_read :
    debug_logging:bool ->
    ?timeout:Watchman_sig.Types.timeout ->
    conn ->
    Yojson.Safe.t option

  val close_connection : conn -> unit

  val get_reader : conn -> Buffered_line_reader.t

  module Testing : sig
    val get_test_conn : unit -> conn
  end
end = struct
  type conn = Buffered_line_reader.t * Out_channel.t

  (** Throw this exception when we know there is something to read from
      the watchman channel, but reading took too long. *)
  exception Read_payload_too_long

  (** Send a request to the watchman process *)
  let send_request ~debug_logging oc json =
    (* unsorted: watchman query, consumed by external tool *)
    let json_str = Yojson.Safe.to_string json in
    if debug_logging then
      Hh_logger.info "Watchman request: %s" json_str
    else
      let has_changed = ref false in
      let json =
        Hh_json_helpers.json_truncate
          json
          ~max_array_elt_count:Helpers.max_array_elt_in_json_logging
          ~has_changed
      in
      Hh_logger.info
        "Watchman request: %s%s"
        (if !has_changed then
          "(truncated) "
        else
          "")
        (* unsorted: watchman query, consumed by external tool *)
        (Yojson.Safe.to_string json);
      Out_channel.output_string oc json_str;
      Out_channel.output_string oc "\n";
      Out_channel.flush oc

  (***************************************************************************)
  (* Handling requests and responses. *)
  (***************************************************************************)

  let has_input timeout reader =
    if Buffered_line_reader.has_buffered_content reader then
      true
    else
      (* Negative means "no timeout" to select *)
      let timeout =
        Option.value (Helpers.timeout_to_secs timeout) ~default:~-.1.
      in
      match
        Sys_utils.select_non_intr
          [Buffered_line_reader.get_fd reader]
          []
          []
          timeout
      with
      | ([], _, _) -> false
      | _ -> true

  let read_with_timeout timeout reader =
    let start_t = Unix.time () in
    if not (has_input timeout reader) then
      raise T.Timeout
    else
      match Helpers.timeout_to_secs timeout with
      | None -> Buffered_line_reader.get_next_line reader
      | Some timeout ->
        let remaining = start_t +. timeout -. Unix.time () in
        let timeout = int_of_float remaining in
        let timeout = max timeout 10 in
        Timeout.with_timeout
          ~do_:(fun _ -> Buffered_line_reader.get_next_line reader)
          ~timeout
          ~on_timeout:(fun (_ : Timeout.timings) ->
            let () = Event_logger.watchman_timeout () in
            raise Read_payload_too_long)

  (** Asks watchman for the path to the socket file *)
  let get_sockname timeout =
    let (ic, pid) =
      Timeout.open_process_in
        Exec_command.Watchman
        [|
          Exec_command.to_string Exec_command.Watchman;
          "get-sockname";
          "--no-pretty";
        |]
    in
    let reader = Buffered_line_reader.create @@ Unix.descr_of_in_channel ic in
    let output = read_with_timeout timeout reader in
    assert (
      match Timeout.close_process_in (ic, pid) with
      | Unix.WEXITED 0 -> true
      | _ -> false);
    let json = Yojson.Safe.from_string output in
    J.get_string_val "sockname" json

  (* Opens a connection to the watchman process through the socket *)
  let open_connection ~timeout ~(sockname : string option) =
    let sockname =
      match sockname with
      | Some sockname -> sockname
      | None -> get_sockname timeout
    in
    let (ic, oc) = Unix.open_connection (Unix.ADDR_UNIX sockname) in
    let reader = Buffered_line_reader.create @@ Unix.descr_of_in_channel ic in
    (reader, oc)

  let close_connection conn =
    let (reader, _) = conn in
    Unix.close @@ Buffered_line_reader.get_fd reader

  (** Open a connection to the watchman socket, call the continuation, then
    * close. *)
  let with_watchman_conn ~timeout ~(sockname : string option) f =
    let conn = open_connection ~timeout ~sockname in
    let result =
      try f conn with
      | exn ->
        let e = Exception.wrap exn in
        Unix.close @@ Buffered_line_reader.get_fd @@ fst conn;
        Exception.reraise e
    in
    Unix.close @@ Buffered_line_reader.get_fd @@ fst conn;
    result

  (** Sends a request to watchman and returns the response. If we don't have a connection,
      a new connection will be created before the request and destroyed after the response *)
  let rec request
      ~debug_logging
      ?conn
      ?(timeout = T.Default_timeout)
      ~(sockname : string option)
      json =
    match conn with
    | None ->
      with_watchman_conn ~timeout ~sockname (fun conn ->
          request ~debug_logging ~conn ~timeout ~sockname json)
    | Some (reader, oc) ->
      send_request ~debug_logging oc json;
      Helpers.sanitize_watchman_response
        ~debug_logging
        (read_with_timeout timeout reader)

  let send_request_and_do_not_wait_for_response
      ~debug_logging ~conn:(_, oc) json =
    send_request ~debug_logging oc json

  let blocking_read ~debug_logging ?(timeout = T.Explicit_timeout 0.) conn =
    let ready = has_input timeout @@ fst conn in
    if not ready then
      match timeout with
      | T.No_timeout -> None
      | T.Explicit_timeout timeout when Float.equal timeout 0. -> None
      | _ -> raise T.Timeout
    else
      (* Use the timeout mechanism to limit maximum time to read payload (cap
       * data size) so we don't freeze if watchman sends an inordinate amount of
       * data, or if it is malformed (i.e. doesn't end in a newline). *)
      let timeout = 40 in
      let output =
        Timeout.with_timeout
          ~do_:(fun _ -> Buffered_line_reader.get_next_line @@ fst conn)
          ~timeout
          ~on_timeout:
            begin
              fun (_ : Timeout.timings) ->
                let () =
                  Hh_logger.log "Watchman_conn.blocking_read timed out"
                in
                raise Read_payload_too_long
            end
      in
      Some (Helpers.sanitize_watchman_response ~debug_logging output)

  let get_reader (reader, _) = reader

  module Testing = struct
    let get_test_conn () =
      (Buffered_line_reader.get_null_reader (), Out_channel.create "/dev/null")
  end
end

(** The production implementation of this Watchman module,
    as opposed to the mock implementation Watchman_mock *)
module Watchman_actual : Watchman_sig.S = struct
  type conn = Watchman_conn.conn

  (** Ocaml module signatures are static. This means since mocking
  functionality is exposed in the mli, it must be implemented for
  both actual builds as well as testing/mocked builds.

  For actual builds, we just raise exceptions for the
  setters. *)
  module Mocking = struct
    exception Cannot_set_when_mocks_disabled

    let print_env _ = raise Cannot_set_when_mocks_disabled

    let get_changes_returns _ = raise Cannot_set_when_mocks_disabled

    let init_returns _ = raise Cannot_set_when_mocks_disabled
  end

  (** This number is totally arbitrary. Just need some cap. *)
  let max_reinit_attempts = 8

  (** Triggers watchman to flush buffered updates on this subscription.
      See watchman/docs/cmd/flush-subscriptions.html *)
  let flush_subscriptions_cmd = "cmd-flush-subscriptions"

  include Watchman_sig.Types

  type dead_env = {
    prior_settings: init_settings;
        (** Will reuse original settings to reinitializing watchman subscription. *)
    reinit_attempts: int;
    dead_since: float;
    prior_clockspec: string;
  }

  type env = {
    mutable clockspec: string;
        (** See https://facebook.github.io/watchman/docs/clockspec.html
            This is also used to reliably detect a crashed watchman. Watchman has a
            facility to detect watchman process crashes between two "since" queries.
            See also assert_no_fresh_instance *)
    conn: Watchman_conn.conn;
    settings: init_settings;
    subscription: string;
    watch_root: string;
    watched_path_expression_terms: Yojson.Safe.t option;
  }

  let dead_env_from_alive env =
    {
      prior_settings = env.settings;
      dead_since = Unix.time ();
      reinit_attempts = 0;
      (* When we start a new watchman connection, we continue to use the prior
       * clockspec. If the same watchman server is still alive, then all is
       * good. If not, the clockspec allows us to detect whether a new watchman
       * server had to be started. See also "is_fresh_instance" on watchman's
       * "since" response. *)
      prior_clockspec = env.clockspec;
    }

  type watchman_instance =
    (* Indicates a dead watchman instance (most likely due to chef upgrading,
     * reconfiguration, or a user terminating watchman, or a timeout reading
     * from the connection) detected by, for example, a pipe error or a timeout.
     *
     * TODO: Currently fallback to a Watchman_dead is only handled in calls
     * wrapped by the with_crash_record. Pipe errors elsewhere (for example
     * during request) will still result in Hack exiting. Need to cover those
     * cases too. *)
    | Watchman_dead of dead_env
    | Watchman_alive of env

  module J = Hh_json_helpers.AdhocJsonHelpers

  (****************************************************************************)
  (* JSON methods. *)
  (****************************************************************************)

  let clock (root : clock) = J.strlist ["clock"; root]

  type watch_command =
    | Subscribe
    | Query

  (** Conjunction of extra_expressions and env.settings.expression_terms. *)
  let request_json
      ?(extra_kv = []) ?(extra_expressions = []) watchman_command env =
    let header =
      let command =
        match watchman_command with
        | Subscribe -> "subscribe"
        | Query -> "query"
      in
      [`String command; `String env.watch_root]
      @
      match watchman_command with
      | Subscribe -> [`String env.subscription]
      | _ -> []
    in
    let directives =
      let expressions = extra_expressions @ env.settings.expression_terms in
      let expressions =
        match env.watched_path_expression_terms with
        | Some terms -> terms :: expressions
        | None -> expressions
      in
      assert (not (List.is_empty expressions));
      [
        `Assoc
          (extra_kv
          @ [
              ("fields", J.strlist ["name"]);
              (* Watchman doesn't allow an empty allof expression. But expressions is never empty *)
              ("expression", J.pred "allof" expressions);
            ]);
      ]
    in
    let request = `List (header @ directives) in
    request

  let get_changes_since_mergebase_query env =
    let extra_kv =
      [
        ( "since",
          `Assoc [("scm", `Assoc [("mergebase-with", `String "master")])] );
      ]
    in
    request_json ~extra_kv Query env

  let since_query env =
    request_json
      ~extra_kv:
        [
          ("since", `String env.clockspec);
          ("empty_on_fresh_instance", `Bool true);
        ]
      Query
      env

  let subscribe ~mode env =
    let (since, mode) =
      match mode with
      | All_changes -> (`String env.clockspec, [])
      | Defer_changes ->
        ( `String env.clockspec,
          [("defer", J.strlist [Hg_states.update; "meerkat-build"])] )
      | Scm_aware ->
        Hh_logger.log "Making Scm_aware subscription";
        let scm = `Assoc [("mergebase-with", `String "master")] in
        let since =
          `Assoc
            [
              ("scm", scm);
              ("drop", J.strlist [Hg_states.update; "meerkat-build"]);
            ]
        in
        (since, [])
    in
    request_json
      ~extra_kv:
        (([("since", since)] @ mode) @ [("empty_on_fresh_instance", `Bool true)])
      Subscribe
      env

  let watch_project root = J.strlist ["watch-project"; root]

  (** See https://facebook.github.io/watchman/docs/cmd/version.html *)
  let capability_check ?(optional = []) required =
    `List
      ([`String "version"]
      @ [
          `Assoc
            [("optional", J.strlist optional); ("required", J.strlist required)];
        ])

  (** We filter all responses from `get_changes` through this. This is to detect
  Watchman server crashes.

  See also Watchman docs on "since" query parameter. *)
  let assert_no_fresh_instance obj =
    Hh_json_helpers.Access.(
      let _ =
        return obj >>= get_bool "is_fresh_instance" >>= fun (is_fresh, trace) ->
        if is_fresh then (
          Hh_logger.log "Watchman server is fresh instance. Exiting.";
          raise Exit_status.(Exit_with Watchman_fresh_instance)
        ) else
          Ok ((), trace)
      in
      ())

  (****************************************************************************)
  (* Initialization, reinitialization, and crash-tracking. *)
  (****************************************************************************)

  let with_crash_record_exn source f =
    try f () with
    | exn ->
      let exn = Exception.wrap exn in
      Hh_logger.exception_ ~prefix:("Watchman " ^ source ^ ": ") exn;
      Exception.reraise exn

  let with_crash_record_opt source f =
    try Some (with_crash_record_exn source f) with
    (* Avoid swallowing these *)
    | (Exit_status.Exit_with _ | Watchman_restarted) as exn ->
      Exception.reraise (Exception.wrap exn)
    | _ -> None

  let has_capability name capabilities =
    (* Projects down from the boolean error monad into booleans.
     * Error states go to false, values are projected directly. *)
    let project_bool m =
      match m with
      | Ok (v, _) -> v
      | Error _ -> false
    in
    Hh_json_helpers.Access.(
      return capabilities
      >>= get_obj "capabilities"
      >>= get_bool name
      |> project_bool)

  (* When we re-init our connection to Watchman, we use the old clockspec to get all the changes
   * since our last response. However, if Watchman has restarted and the old clockspec pre-dates
   * the new Watchman, then we may miss updates. It is important for Flow and Hack to restart
   * in that case.
   *
   * Unfortunately, the response to "subscribe" doesn't have the "is_fresh_instance" field. So
   * we'll instead send a small "query" request. It should always return 0 files, but it should
   * tell us whether the Watchman service has restarted since clockspec.
   *)
  let assert_watchman_has_not_restarted_since
      ~debug_logging ~conn ~sockname ~watch_root ~clockspec =
    let hard_to_match_name = "irrelevant.potato" in
    let query =
      `List
        [
          `String "query";
          `String watch_root;
          `Assoc
            [
              ("since", `String clockspec);
              ("empty_on_fresh_instance", `Bool true);
              ("expression", `List [`String "name"; `String hard_to_match_name]);
            ];
        ]
    in
    let response = Watchman_conn.request ~debug_logging ~conn ~sockname query in
    match Hh_json_helpers.Jget.bool_opt (Some response) "is_fresh_instance" with
    | Some false -> ()
    | Some true ->
      Hh_logger.error
        "Watchman server restarted so we may have missed some updates";
      raise Watchman_restarted
    | None ->
      (* The response to this query **always** should include the `is_fresh_instance` boolean
       * property. If it is missing then something has gone wrong with Watchman. Since we can't
       * prove that Watchman has not restarted, we must treat this as an error. *)
      Hh_logger.error
        "Invalid Watchman response to `empty_on_fresh_instance` query:\n%s"
        (* unsorted: watchman query, consumed by external tool *)
        (Yojson.Safe.pretty_to_string response);
      raise Exit_status.(Exit_with Watchman_failed)

  (** Given a list of terms [a;b;c], this constructs [ ["dirname","relative_path"]; a; b; c]
  It's used because if the watchman root is /fbsource but we want to watch /fbsource/www,
  then watchman needs us to pass "/fbsource" as the watched-root and then use our
  own dirname query to restrict to the directory we want.

  CARE: watchman is fast with [dirname,relative_path] and [alloff, [dirname, relative_path]]
  but it's slow with [anyof, [dirname,relative_path], [name,relative_path]]. That's because
  watchman indexes on dirname and the first two queries can be done fast, but the third
  query would require an item-by-item search over every item in /fbsource to see if it
  matches the "name" predicate.

  This function doesn't do anything if relative_path is empty or there are no terms, then
  we don't add the restrictions (since it wouldn't be needed). *)
  let prepend_relative_path_of_directory ~relative_path ~terms =
    match terms with
    | None -> None
    | Some _ when String.equal relative_path "" ->
      (* If we're watching the watch root directory, then there's no point in specifying a list of
       * files and directories to watch. We're already subscribed to any change in this watch root
       * anyway *)
      None
    | Some terms ->
      (* So lets say we're being told to watch foo/bar. Is foo/bar a directory? Is it a file? If it
       * is a file now, might it become a directory later? I'm not aware of aterm which will watch for either a file or a directory, so let's add two terms *)
      Some (J.strlist ["dirname"; relative_path] :: terms)

  let re_init
      ?prior_clockspec
      {
        init_timeout;
        subscribe_mode;
        expression_terms;
        debug_logging;
        roots;
        sockname;
        subscription_prefix;
      } =
    with_crash_record_opt "init" @@ fun () ->
    let conn = Watchman_conn.open_connection ~timeout:init_timeout ~sockname in
    let capabilities =
      Watchman_conn.request
        ~debug_logging
        ~conn
        ~timeout:Default_timeout
        ~sockname
        (capability_check ~optional:[flush_subscriptions_cmd] ["relative_root"])
    in
    let supports_flush = has_capability flush_subscriptions_cmd capabilities in
    (* Disable subscribe if Watchman flush feature isn't supported. *)
    let subscribe_mode =
      if supports_flush then
        subscribe_mode
      else
        None
    in
    let (watched_path_expression_terms, watch_roots, failed_paths) =
      List.fold
        roots
        ~init:(Some [], S_set.empty, S_set.empty)
        ~f:(fun (terms, watch_roots, failed_paths) path ->
          (* Watch this root. If the path doesn't exist, watch_project will throw. In that case catch
           * the error and continue for now. *)
          let response =
            try
              Some
                (Watchman_conn.request
                   ~debug_logging
                   ~conn
                   ~sockname
                   (watch_project (Path.to_string path)))
            with
            | _ -> None
          in
          match response with
          | None ->
            (terms, watch_roots, S_set.add (Path.to_string path) failed_paths)
          | Some response ->
            let watch_root = J.get_string_val "watch" response in
            let relative_path =
              J.get_string_val "relative_path" ~default:"" response
            in
            let terms =
              prepend_relative_path_of_directory ~relative_path ~terms
            in
            let watch_roots = S_set.add watch_root watch_roots in
            (terms, watch_roots, failed_paths))
    in
    (* The failed_paths are likely includes which don't exist on the filesystem, so watch_project
     * returned an error. Let's do a best effort attempt to infer the watch root and relative
     * path for each bad include *)
    let watched_path_expression_terms =
      S_set.fold
        (fun path terms ->
          String_utils.(
            match
              S_set.find_first_opt
                (fun root -> String.is_prefix path ~prefix:root)
                watch_roots
            with
            | None -> failwith (spf "Cannot deduce watch root for path %s" path)
            | Some root ->
              let relative_path = lstrip (lstrip path root) Filename.dir_sep in
              prepend_relative_path_of_directory ~relative_path ~terms))
        failed_paths
        watched_path_expression_terms
    in
    (* All of our watched paths should have the same watch root. Let's assert that *)
    let watch_root =
      match S_set.elements watch_roots with
      | [] -> failwith "Cannot run watchman with fewer than 1 root"
      | [watch_root] -> watch_root
      | _ ->
        failwith
          (spf
             "Can't watch paths across multiple Watchman watch_roots. Found %d watch_roots"
             (S_set.cardinal watch_roots))
    in
    (* If we don't have a prior clockspec, grab the current clock *)
    let clockspec =
      match prior_clockspec with
      | Some clockspec ->
        assert_watchman_has_not_restarted_since
          ~debug_logging
          ~conn
          ~sockname
          ~watch_root
          ~clockspec;
        clockspec
      | None ->
        Watchman_conn.request ~debug_logging ~conn ~sockname (clock watch_root)
        |> J.get_string_val "clock"
    in
    let watched_path_expression_terms =
      Option.map watched_path_expression_terms ~f:(fun terms ->
          J.pred "anyof" terms)
    in
    let env =
      {
        settings =
          {
            init_timeout;
            debug_logging;
            subscribe_mode;
            expression_terms;
            roots;
            sockname;
            subscription_prefix;
          };
        conn;
        watch_root;
        watched_path_expression_terms;
        clockspec;
        subscription =
          Printf.sprintf "%s.%d" subscription_prefix (Unix.getpid ());
      }
    in
    (match subscribe_mode with
    | None -> ()
    | Some mode ->
      let _response : Yojson.Safe.t =
        Watchman_conn.request
          ~debug_logging
          ~conn
          ~sockname
          (subscribe ~mode env)
      in
      ());
    env

  let init ?since_clockspec settings () =
    let prior_clockspec = since_clockspec in
    re_init ?prior_clockspec settings

  let extract_file_names env json =
    let files =
      try J.get_array_val "files" json with
      (* When an hg.update happens, it shows up in the watchman subscription
       * as a notification with no files key present. *)
      | J.Not_found -> []
    in
    let files =
      List.map files ~f:(fun json ->
          let s = Yojson.Safe.Util.to_string json in
          let abs = Filename.concat env.watch_root s in
          abs)
    in
    files

  let within_backoff_time attempts time =
    let offset =
      4.0
      *. 2.0
         ** float
              (if attempts > 3 then
                3
              else
                attempts)
    in
    Float.(Unix.time () >= time +. offset)

  let maybe_restart_instance instance =
    match instance with
    | Watchman_alive _ -> instance
    | Watchman_dead dead_env ->
      if dead_env.reinit_attempts >= max_reinit_attempts then
        let () =
          Hh_logger.log "Ran out of watchman reinit attempts. Exiting."
        in
        raise Exit_status.(Exit_with Watchman_failed)
      else if within_backoff_time dead_env.reinit_attempts dead_env.dead_since
      then (
        let () =
          Hh_logger.log "Attemping to reestablish watchman subscription"
        in
        match
          re_init
            ~prior_clockspec:dead_env.prior_clockspec
            dead_env.prior_settings
        with
        | None ->
          Hh_logger.log "Reestablishing watchman subscription failed.";
          Event_logger.watchman_connection_reestablishment_failed ();
          Watchman_dead
            { dead_env with reinit_attempts = dead_env.reinit_attempts + 1 }
        | Some env ->
          Hh_logger.log "Watchman connection reestablished.";
          Event_logger.watchman_connection_reestablished ();
          Watchman_alive env
      ) else
        instance

  let close env = Watchman_conn.close_connection env.conn

  let close_channel_on_instance env =
    close env;
    Event_logger.watchman_died_caught ();
    (Watchman_dead (dead_env_from_alive env), Watchman_unavailable)

  let with_instance instance ~try_to_restart ~on_alive ~on_dead =
    match
      if try_to_restart then
        maybe_restart_instance instance
      else
        instance
    with
    | Watchman_dead dead_env -> on_dead dead_env
    | Watchman_alive env -> on_alive env

  (** Calls f on the instance, maybe restarting it if its dead and maybe
   * reverting it to a dead state if things go south. For example, if watchman
   * shuts the connection on us, or shuts down, or crashes, we revert to a dead
   * instance, upon which a restart will be attempted down the road.
   * Alternatively, we also proactively revert to a dead instance if it appears
   * to be unresponsive (Timeout), and if reading the payload from it is
   * taking too long. *)
  let call_on_instance =
    let on_dead dead_env = (Watchman_dead dead_env, Watchman_unavailable) in
    let on_alive source f env =
      try
        let (env, result) = with_crash_record_exn source (fun () -> f env) in
        (Watchman_alive env, result)
      with
      | Sys_error msg when String.equal msg "Broken pipe" ->
        Hh_logger.log "Watchman Pipe broken.";
        close_channel_on_instance env
      | Sys_error msg when String.equal msg "Connection reset by peer" ->
        Hh_logger.log "Watchman connection reset by peer.";
        close_channel_on_instance env
      | Sys_error msg when String.equal msg "Bad file descriptor" ->
        (* This happens when watchman is tearing itself down after we
         * retrieved a sock address and connected to the sock address. That's
         * because Unix.open_connection doesn't
         * error even when the sock address is no longer valid and actually -
         * it returns a channel that will error at some later time when you
         * actually try to do anything with it (write to it, or even get the
         * file descriptor of it). I'm pretty sure we don't need to close the
         * channel when that happens since we never had a useable channel
         * to start with. *)
        Hh_logger.log "Watchman bad file descriptor.";
        Event_logger.watchman_died_caught ();

        (Watchman_dead (dead_env_from_alive env), Watchman_unavailable)
      | End_of_file ->
        Hh_logger.log "Watchman connection End_of_file. Closing channel";
        close_channel_on_instance env
      | Watchman_conn.Read_payload_too_long ->
        Hh_logger.log "Watchman reading payload too long. Closing channel";
        close_channel_on_instance env
      | Timeout ->
        Hh_logger.log "Watchman reading Timeout. Closing channel";
        close_channel_on_instance env
      | Watchman_error msg ->
        Hh_logger.log "Watchman error: %s. Closing channel" msg;
        close_channel_on_instance env
      | exn ->
        let msg = Exception.to_string (Exception.wrap exn) in
        Event_logger.watchman_uncaught_failure msg;
        raise Exit_status.(Exit_with Watchman_failed)
    in
    fun instance source f ->
      with_instance
        instance
        ~try_to_restart:true
        ~on_dead
        ~on_alive:(on_alive source f)

  (** This is a large >50MB payload, which could take longer than 2 minutes for
       Watchman to generate and push down the channel. *)
  let get_all_files env : string list =
    try
      with_crash_record_exn "get_all_files" @@ fun () ->
      let response =
        Watchman_conn.request
          ~debug_logging:env.settings.debug_logging
          ~timeout:Default_timeout
          ~sockname:env.settings.sockname
          (request_json ~extra_expressions:[`String "exists"] Query env)
      in
      (* WARNING:
          Updating the clockspec to the one we got from the response above has the
          following consequences, *but only if `subscribe_mode` is None*.
          If any file changes happened in between the last call to `get_changes`
          or `get_changes_synchronously` and the request above, they are
          effectively discarded and will never be reported by subsequent calls
          to these two functions! *)
      env.clockspec <- J.get_string_val "clock" response;
      extract_file_names env response
    with
    | _ -> raise Exit_status.(Exit_with Watchman_failed)

  module RepoStates : sig
    type state = string

    val enter : state -> unit

    val leave : state -> unit

    (** Returns (current_states, past_states).
        [past_states] maps each state name to the [Unix.gettimeofday] timestamp
        of the last time it was left. *)
    val get : unit -> string list * float S_map.t
  end = struct
    type state = string

    type t = {
      past_states: float S_map.t;
          (** Maps state name to [Unix.gettimeofday] timestamp of last leave *)
      current_states: state list;
    }

    let init : t = { past_states = S_map.empty; current_states = [] }

    let states : t ref = ref init

    let enter : state -> unit =
     fun state ->
      states :=
        { !states with current_states = state :: !states.current_states }

    let rec remove_first : string list -> string -> string list =
     fun list elt ->
      match list with
      | [] -> []
      | hd :: list ->
        if String.equal hd elt then
          list
        else
          hd :: remove_first list elt

    let leave : state -> unit =
     fun state ->
      let { current_states; past_states } = !states in
      let current_states = remove_first current_states state in
      let past_states = S_map.add state (Unix.gettimeofday ()) past_states in
      states := { current_states; past_states }

    let get () =
      let { current_states; past_states } = !states in
      (current_states, past_states)
  end

  let make_state_change_response
      (state : [< `Enter | `Leave ]) (name : string) (data : Yojson.Safe.t) :
      pushed_changes =
    let metadata = J.try_get_val "metadata" data in
    match state with
    | `Enter ->
      RepoStates.enter name;
      State_enter (name, metadata)
    | `Leave ->
      RepoStates.leave name;
      State_leave (name, metadata)

  (** returns (data.clock.scm.mergebase, data.since?.scm.mergebase).
  The watchman docs explain: "The since field holds the fat clock that was returned
  in the clock field from the prior subscription update. It is present as a convenience
  for you; you can compare the mergebase fields between the two to determine that the
  merge base changed in this update."
  Edge cases: (1) when we send the first subscribe request, we get back an answer that
  lacks the since property entirely, (2) the next thing we hear has an empty since.mergebase,
  (3) the next thing we hear has a full since.mergebase. *)
  let extract_mergebase data : (Hg.Rev.t * Hg.Rev.t option option) option =
    let open Hh_json_helpers.Access in
    let accessor = return data in
    let ret =
      accessor >>= get_obj "clock" >>= get_obj "scm" >>= get_string "mergebase"
      >>= fun (mergebase, _) ->
      let since_mergebase =
        accessor
        >>= get_obj "since"
        >>= get_obj "scm"
        >>= get_string "mergebase"
        |> to_option
      in
      return
        ( Hg.Rev.of_string mergebase,
          since_mergebase |> Option.map ~f:Hg.Rev.of_string_check_empty )
    in
    to_option ret

  (** watchman's data.clock property is either a slim-clock "string", or a fat-clock "{clock, scm}".
  This function extracts either. *)
  let extract_clock data =
    let open Hh_json_helpers.Access in
    let fat = return data >>= get_obj "clock" >>= get_string "clock" in
    let slim = return data >>= get_string "clock" in
    match (fat, slim) with
    | (Ok (clock, _), _)
    | (_, Ok (clock, _)) ->
      clock
    | _ -> failwith "watchman response lacks clock"

  let transform_asynchronous_get_changes_response env data =
    match data with
    | None -> (env, Files_changed S_set.empty)
    | Some data ->
      let clock = extract_clock data in
      env.clockspec <- clock;
      let files = set_of_list @@ extract_file_names env data in
      (match extract_mergebase data with
      | Some (mergebase, Some None) ->
        (env, Changed_merge_base (mergebase, files, clock))
      | Some (mergebase, Some (Some since_mergebase))
        when not (Hg.Rev.equal mergebase since_mergebase) ->
        (env, Changed_merge_base (mergebase, files, clock))
      | _ ->
        assert_no_fresh_instance data;
        (try
           ( env,
             make_state_change_response
               `Enter
               (J.get_string_val "state-enter" data)
               data )
         with
        | J.Not_found ->
          (try
             ( env,
               make_state_change_response
                 `Leave
                 (J.get_string_val "state-leave" data)
                 data )
           with
          | J.Not_found -> (env, Files_changed files))))

  let get_clock instance =
    match instance with
    | Watchman_alive { clockspec; _ } -> clockspec
    | Watchman_dead { prior_clockspec; _ } -> prior_clockspec

  let get_changes ?deadline (instance : watchman_instance) :
      watchman_instance * changes =
    call_on_instance instance "get_changes" @@ fun env ->
    let timeout =
      Option.map deadline ~f:(fun deadline ->
          let timeout = deadline -. Unix.time () in
          Explicit_timeout Float.(max timeout 0.0))
    in
    let debug_logging = env.settings.debug_logging in
    let sockname = env.settings.sockname in
    if Option.is_some env.settings.subscribe_mode then
      let response =
        Watchman_conn.blocking_read ~debug_logging ?timeout env.conn
      in
      let (env, result) =
        transform_asynchronous_get_changes_response env response
      in
      (env, Watchman_pushed result)
    else
      let query = since_query env in
      let response =
        Watchman_conn.request
          ~debug_logging
          ~conn:env.conn
          ?timeout
          ~sockname
          query
      in
      let (env, changes) =
        transform_asynchronous_get_changes_response env (Some response)
      in
      (env, Watchman_synchronous [changes])

  let get_changes_since_mergebase ?timeout env =
    Watchman_conn.request
      ?timeout
      ~debug_logging:env.settings.debug_logging
      ~sockname:env.settings.sockname
      (get_changes_since_mergebase_query env)
    |> extract_file_names env

  let get_mergebase ?timeout env : Hg.Rev.t =
    let response =
      Watchman_conn.request
        ?timeout
        ~debug_logging:env.settings.debug_logging
        ~sockname:env.settings.sockname
        (get_changes_since_mergebase_query env)
    in
    match extract_mergebase response with
    | Some (mergebase, _since_mergebase) -> mergebase
    | None -> raise (Watchman_error "Failed to extract mergebase from response")

  let flush_request ~(timeout : int) watch_root =
    let directive =
      `Assoc
        [
          (* Watchman expects timeout milliseconds. *)
          ("sync_timeout", `Intlit (string_of_int @@ (timeout * 1000)));
        ]
    in
    `List [`String "flush-subscriptions"; `String watch_root; directive]

  let rec poll_until_sync ~deadline env acc =
    let is_finished_flush_response json =
      match json with
      | None -> false
      | Some json ->
        Hh_json_helpers.Access.(
          let is_synced =
            lazy
              (return json >>= get_array "synced" |> function
               | Error _ -> false
               | Ok (vs, _) ->
                 List.exists vs ~f:(fun str ->
                     String.equal
                       (Hh_json_helpers.get_string_exn str)
                       env.subscription))
          in
          let is_not_needed =
            lazy
              (return json >>= get_array "no_sync_needed" |> function
               | Error _ -> false
               | Ok (vs, _) ->
                 List.exists vs ~f:(fun str ->
                     String.equal
                       (Hh_json_helpers.get_string_exn str)
                       env.subscription))
          in
          Lazy.force is_synced || Lazy.force is_not_needed)
    in
    let timeout =
      let timeout = deadline -. Unix.time () in
      if Float.(timeout <= 0.0) then
        raise Timeout
      else
        Explicit_timeout timeout
    in
    let debug_logging = env.settings.debug_logging in
    let json = Watchman_conn.blocking_read ~debug_logging ~timeout env.conn in
    if is_finished_flush_response json then
      (env, acc)
    else
      let (env, acc) =
        match json with
        | None -> (env, acc)
        | Some json ->
          let (env, result) =
            transform_asynchronous_get_changes_response env (Some json)
          in
          (env, result :: acc)
      in
      poll_until_sync ~deadline env acc

  let poll_until_sync ~deadline env = poll_until_sync ~deadline env []

  let get_changes_synchronously ~(timeout : int) instance =
    let (instance, status) =
      call_on_instance instance "get_changes_synchronously" @@ fun env ->
      if Option.is_none env.settings.subscribe_mode then
        let timeout = Explicit_timeout (float timeout) in
        let query = since_query env in
        let response =
          Watchman_conn.request
            ~debug_logging:env.settings.debug_logging
            ~conn:env.conn
            ~timeout
            ~sockname:env.settings.sockname
            query
        in
        let (env, changes) =
          transform_asynchronous_get_changes_response env (Some response)
        in
        (env, Watchman_synchronous [changes])
      else
        let request = flush_request ~timeout env.watch_root in
        let conn = env.conn in
        Watchman_conn.send_request_and_do_not_wait_for_response
          ~debug_logging:env.settings.debug_logging
          ~conn
          request;
        let deadline = Unix.time () +. float_of_int timeout in
        let (env, changes) = poll_until_sync ~deadline env in
        (env, Watchman_synchronous (List.rev changes))
    in
    match status with
    | Watchman_unavailable ->
      raise (Watchman_error "Watchman unavailable for synchronous response")
    | Watchman_pushed _ ->
      raise (Watchman_error "Wtf? pushed response from synchronous request")
    | Watchman_synchronous files -> (instance, files)

  let conn_of_instance = function
    | Watchman_dead _ -> None
    | Watchman_alive { conn; _ } -> Some conn

  let get_reader instance =
    Option.map (conn_of_instance instance) ~f:Watchman_conn.get_reader

  module Testing = struct
    include Testing_common

    let get_test_env () =
      let conn = Watchman_conn.Testing.get_test_conn () in
      {
        settings = test_settings;
        conn;
        watch_root = "/path/to/root";
        clockspec = "";
        watched_path_expression_terms =
          Some
            (J.pred
               "anyof"
               [J.strlist ["dirname"; "foo"]; J.strlist ["name"; "foo"]]);
        subscription = "dummy_prefix.123456789";
      }

    let transform_asynchronous_get_changes_response env json =
      transform_asynchronous_get_changes_response env json
  end
end

module Watchman_mock = struct
  exception Not_available_in_mocking

  type conn

  include Watchman_sig.Types

  type env = string

  type dead_env = unit

  type watchman_instance =
    | Watchman_dead of dead_env
    | Watchman_alive of env

  module Mocking = struct
    let print_env env = env

    let init = ref None

    let init_returns v = init := v

    let changes = ref Watchman_unavailable

    let get_changes_returns v = changes := v

    let changes_synchronously = ref []

    let all_files = ref []
  end

  module Testing = struct
    include Testing_common

    let get_test_env () = "test_env"

    let transform_asynchronous_get_changes_response _ _ =
      raise Not_available_in_mocking
  end

  module RepoStates = struct
    let get () : string list * float S_map.t = ([], S_map.empty)
  end

  let init ?since_clockspec:_ _ () = !Mocking.init

  let get_changes ?deadline instance =
    let _ = deadline in
    let result = !Mocking.changes in
    Mocking.changes := Watchman_unavailable;
    (instance, result)

  let get_changes_synchronously ~timeout instance =
    let _ = timeout in
    let result = !Mocking.changes_synchronously in
    Mocking.changes_synchronously := [];
    (instance, result)

  let get_clock _instance = ""

  let get_reader _ = None

  let conn_of_instance _ = None

  let get_all_files _ =
    let result = !Mocking.all_files in
    Mocking.all_files := [];
    result

  let get_changes_since_mergebase ?timeout:_ _ = []

  let get_mergebase ?timeout:_ _ : Hg.Rev.t = Hg.Rev.of_string "mergebase"

  let close _ = ()

  let with_instance instance ~try_to_restart:_ ~on_alive ~on_dead =
    match instance with
    | Watchman_dead dead_env -> on_dead dead_env
    | Watchman_alive env -> on_alive env
end

(** Helpers to invoke the `watchman` executable directly. *)
module Process (Exec : Watchman_sig.Exec) = struct
  type error =
    | Process_failure of Exec.error
    | Unexpected_json of { json_string: string }

  let error_to_string = function
    | Process_failure e ->
      Printf.sprintf
        "Error while running `watchman` command: %s"
        (Exec.error_to_string e)
    | Unexpected_json { json_string } ->
      Printf.sprintf "Error while parsing watchman response: %s" json_string

  (** [watch_project ~root ~socket] queries watchman to watch a [root]. *)
  let watch_project ~root ~sockname =
    let open Exec.Monad_infix in
    let args =
      ["watch-project"; Path.to_string root]
      @
      match sockname with
      | None -> []
      | Some sockname -> ["--sockname"; Path.to_string sockname]
    in
    Exec.exec Exec_command.Watchman args
    >>| Result.map_error ~f:(fun e -> Process_failure e)
    >|= fun stdout ->
    try
      Ok
        (Watchman_sig.Types.watch_project_response_of_yojson
        @@ Yojson.Safe.from_string stdout)
    with
    | Ppx_yojson_conv_lib.Yojson_conv.Of_yojson_error _ ->
      Error (Unexpected_json { json_string = stdout })
end

include
  (val if
         Injector_config.use_test_stubbing
         && not (Injector_config.Exclusions.exclude_watchman ())
       then
         (module Watchman_mock : Watchman_sig.S)
       else
         (module Watchman_actual : Watchman_sig.S))
