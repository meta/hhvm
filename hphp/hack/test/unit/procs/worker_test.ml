open Hh_prelude

let entry =
  Worker_controller_entry_point.register ~restore:(fun () ~(worker_id : int) ->
      Hh_logger.set_id (Printf.sprintf "worker_test %d" worker_id))

let num_workers = 2

let make_worker ?call_wrapper ~longlived_workers heap_handle =
  Worker_controller.make
    ?call_wrapper
    ~longlived_workers
    ~saved_state:()
    ~entry
    num_workers
    ~gc_control:(Gc.get ())
    ~heap_handle

let rec wait_until_ready handle =
  let { Worker_controller.readys; waiters = _; ready_fds = _ } =
    Worker_controller.select [handle] []
  in
  match readys with
  | [] -> wait_until_ready handle
  | ready :: _ -> ready

(** If "f x" throws, we exit the program with a custom exit code. *)
let catch_exception_and_custom_exit_wrapper : 'x 'b. ('x -> 'b) -> 'x -> 'b =
 fun f x ->
  try f x with
  | _ -> exit 17

let call_and_verify_result worker f x expected =
  let result =
    Worker_controller.call worker f x
    |> wait_until_ready
    |> Worker_controller.get_result
  in
  String.equal result expected

(** This is just like the test_worker_uncaught_exception_exits_with_2 test
 * except we add a call_wapper to the worker. It catches all exceptions and
 * makes the worker exit with code 17. *)
let test_wrapped_worker_with_custom_exit use_clones heap_handle () =
  let workers =
    make_worker
      ~call_wrapper:
        (Worker_controller.Call_wrapper
           {
             prepare = (fun () -> ());
             wrap = (fun () f x -> catch_exception_and_custom_exit_wrapper f x);
           })
      ~longlived_workers:(not use_clones)
      heap_handle
  in
  match workers with
  | [] ->
    Printf.eprintf "Failed to create workers";
    false
  | worker :: _ ->
    (try
       call_and_verify_result
         worker
         (fun () -> raise (Failure "oops"))
         ()
         "dummy"
     with
    | Worker_controller.Worker_failed
        (_, Worker_controller.Worker_quit (Unix.WEXITED i)) ->
      i = 17)

let test_worker_uncaught_exception_exits_with_2 use_clones heap_handle () =
  let workers = make_worker ~longlived_workers:use_clones heap_handle in
  match workers with
  | [] ->
    Printf.eprintf "Failed to create workers";
    false
  | worker :: _ ->
    (try
       call_and_verify_result
         worker
         (fun () -> raise (Failure "oops"))
         ()
         "dummy"
     with
    | Worker_controller.Worker_failed
        (_, Worker_controller.Worker_quit (Unix.WEXITED i)) ->
      i = 2)

let test_job_marshal_exception_exits_with_2 use_clones heap_handle () =
  let workers = make_worker ~longlived_workers:use_clones heap_handle in
  match workers with
  | [] ->
    Printf.eprintf "Failed to create workers";
    false
  | worker :: _ ->
    (try
       call_and_verify_result
         worker
         (fun () -> raise Marshal_tools.Reading_Payload_Exception)
         ()
         "dummy"
     with
    | Worker_controller.Worker_failed
        (_, Worker_controller.Worker_quit (Unix.WEXITED i)) ->
      i = 2)

let test_controller_channel_closure_is_controller_death () =
  let (input_fd, controller_fd) = Unix.pipe () in
  let output_fd = Daemon.null_fd () in
  let controller_fd_is_open = ref true in
  let close_no_fail fd =
    try Unix.close fd with
    | Unix.Unix_error (Unix.EBADF, _, _) -> ()
  in
  let close_controller_fd () =
    if !controller_fd_is_open then (
      controller_fd_is_open := false;
      close_no_fail controller_fd
    )
  in
  Utils.try_finally
    ~f:(fun () ->
      let preamble = Marshal_tools.make_preamble 2 in
      let preamble_size = Bytes.length preamble in
      let preamble_bytes_written =
        Unix.write controller_fd preamble 0 preamble_size
      in
      let payload = Bytes.of_string "x" in
      let payload_bytes_written = Unix.write controller_fd payload 0 1 in
      close_controller_fd ();
      let outcome =
        Utils.try_finally
          ~f:(fun () -> Worker.For_test.read_and_process_job input_fd output_fd)
          ~finally:(fun () -> Measure.pop_global () |> ignore)
      in
      preamble_bytes_written = preamble_size
      && payload_bytes_written = 1
      && Poly.(outcome = `Controller_has_died))
    ~finally:(fun () ->
      close_no_fail input_fd;
      close_controller_fd ();
      close_no_fail output_fd)

let test_simple_worker_spawn use_clones heap_handle () =
  let workers = make_worker ~longlived_workers:use_clones heap_handle in
  match workers with
  | [] ->
    Printf.eprintf "Failed to create workers";
    false
  | worker :: _ -> call_and_verify_result worker (fun () -> "hello") () "hello"

let worker_prepared_data : (int * int * string) option ref = ref None

(** Tests that:
    - Preparation runs once in the master for each request, observing changes
      made in the master after pool creation and between calls.
    - The preparation function itself is not marshalled *)
let test_wrapper_prepares_each_request use_clones heap_handle () =
  let master_pid = Unix.getpid () in
  let master_input = Stdlib.open_in "/dev/null" in
  let value = ref "pool creation" in
  let preparations = ref 0 in
  let created_workers = ref [] in
  let call_wrapper =
    Worker_controller.Call_wrapper
      {
        prepare =
          (fun () ->
            (* Check that this runs in the master. *)
            assert (Unix.getpid () = master_pid);
            (* Refer to the channel inside [prepare] so its closure captures it.
               If we accidentally tried to marshall the [prepare] function, this
               would make marshalling fail. *)
            assert (Poly.(Unix.descr_of_in_channel master_input <> Unix.stdin));
            incr preparations;
            (master_pid, !preparations, !value));
        wrap =
          (fun data f x ->
            (* Check that we are not in the master process. *)
            assert (Unix.getpid () <> master_pid);
            (* The job reads this worker-local ref to check that the wrapper
               received the snapshot and ran before the job. *)
            worker_prepared_data := Some data;
            f x);
      }
  in
  Utils.try_finally
    ~f:(fun () ->
      let workers =
        make_worker
          ~call_wrapper
          ~longlived_workers:(not use_clones)
          heap_handle
      in
      created_workers := workers;
      match workers with
      | [] -> false
      | worker :: _ ->
        (* Pool creation must not take the snapshot. *)
        let prepared_at_call_time = Int.equal !preparations 0 in
        (* The result encodes the preparation count, captured value, and whether
           preparation and job execution happened in different processes. *)
        let read_prepared_data () =
          match !worker_prepared_data with
          | None -> "wrapper did not run"
          | Some (prepared_pid, count, captured_value) ->
            Printf.sprintf
              "%d:%s:%b"
              count
              captured_value
              (prepared_pid <> Unix.getpid ())
        in
        (* Changing the value after pool creation checks when it is captured.
           The counter must increment in the master exactly once per call. *)
        value := "first request";
        let first =
          call_and_verify_result
            worker
            read_prepared_data
            ()
            "1:first request:true"
          && Int.equal !preparations 1
        in
        (* Reuse the same worker with a new value: preparation must run again
           rather than reuse the first request's snapshot. *)
        value := "second request";
        let second =
          call_and_verify_result
            worker
            read_prepared_data
            ()
            "2:second request:true"
          && Int.equal !preparations 2
        in
        prepared_at_call_time && first && second)
    ~finally:(fun () ->
      Stdlib.close_in master_input;
      List.iter !created_workers ~f:(fun worker ->
          Daemon.force_quit (Worker_controller.spawn worker)))

(** A failed preparation sends no request and leaves the worker available for
    the next call. The exception is raised in the master, before job execution. *)
let test_wrapper_preparation_failure_leaves_worker_available
    use_clones heap_handle () =
  let master_pid = Unix.getpid () in
  let fail_preparation = ref true in
  let preparations = ref 0 in
  let created_workers = ref [] in
  let call_wrapper =
    Worker_controller.Call_wrapper
      {
        prepare =
          (fun () ->
            assert (Unix.getpid () = master_pid);
            incr preparations;
            if !fail_preparation then failwith "test wrapper preparation";
            ());
        wrap = (fun () f x -> f x);
      }
  in
  Utils.try_finally
    ~f:(fun () ->
      let workers =
        make_worker
          ~call_wrapper
          ~longlived_workers:(not use_clones)
          heap_handle
      in
      created_workers := workers;
      match workers with
      | [] -> false
      | worker :: _ ->
        let rejected =
          try
            let (_ : (unit, string) Worker_controller.handle) =
              Worker_controller.call worker (fun () -> "not sent") ()
            in
            false
          with
          | Failure message -> String.equal message "test wrapper preparation"
        in
        (* A subsequent request must reach the same worker successfully. *)
        fail_preparation := false;
        let reused =
          call_and_verify_result
            worker
            (fun () -> "after failure")
            ()
            "after failure"
        in
        rejected && reused && Int.equal !preparations 2)
    ~finally:(fun () ->
      List.iter !created_workers ~f:(fun worker ->
          Daemon.force_quit (Worker_controller.spawn worker)))

let make_tests handle =
  let make_test name fn =
    [(name, fn true handle); ("no_clones_" ^ name, fn false handle)]
  in
  make_test "simple_worker_spawn_test" test_simple_worker_spawn
  @ make_test
      "worker_uncaught_exception_exits_with_2"
      test_worker_uncaught_exception_exits_with_2
  @ make_test
      "job_marshal_exception_exits_with_2"
      test_job_marshal_exception_exits_with_2
  @ make_test
      "wrapped_worker_with_custom_exit"
      test_wrapped_worker_with_custom_exit
  @ make_test "wrapper_prepares_each_request" test_wrapper_prepares_each_request
  @ make_test
      "wrapper_preparation_failure_leaves_worker_available"
      test_wrapper_preparation_failure_leaves_worker_available
  @ [
      ( "controller_channel_closure_is_controller_death",
        test_controller_channel_closure_is_controller_death );
    ]

let () =
  Daemon.check_entry_point ();

  (* this call might not return *)
  let heap_handle = Shared_mem.init ~num_workers Shared_mem.default_config in
  let tests = make_tests heap_handle in
  Unit_test.run_all tests
