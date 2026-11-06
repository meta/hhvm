(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

module Hack_bucket = Bucket
open Hh_prelude
open Option.Monad_infix
module Bucket = Hack_bucket
open Typing_service_types

(*
####

The type checking service receives a list of files and their symbols as input and
distributes the work to worker processes.

A worker process's input is a subset of files. At the end of its work,
it returns back to master the following progress report:
  - completed: a list of computations it completed
  - remaining: a list of computations which it didn't end up performing. For example,
    it may not have been able to get through the whole list of computations because
    it exceeded its worker memory cap
  - deferred: a list of computations which were discovered to be necessary to be
    performed before some of the computations in the input subset

The deferred list needs further explanation, so read below.

####

Here's how we actually plumb through the workitems and their individual results, via Multi_worker:
* We have mutable state [ref files_to_process], a list of filenames still to be processed
    for the entire typecheck.
* Datatype [progress] represents an input batch of work for a worker to do,
    and also an output indication of how it dispatched that batch
* Datatype [typing_result] represents the accumulation of telemetry, errors, deps results from workers.
    Each individual worker is given an empty [typing_result] as input,
    and merges its own results into that empty input to give a per-worker output,
    and then we have a separate accumulator and we merge each per-worker's output
    into that accumulator.
* next : () -> progress
    This mutates [files_to_process] by removing a bucket of filenames,
    and returns a degenerate progress {remaining=bucket; completed=[]; deferred=[]}
    which is basically just the bucket of work to be done by the job.
* neutral : typing_result
    This value is just the empty typing_result {diagnostics=Empty; deps=Empty; telemetry=Empty}
* job : typing_result -> progress -> (typing_result', progress')
    Multi_worker will invoke this job. For input,
    it provides a copy of the degenerate [typing_result] that it got from [neutral], and
    it provides the degenerate [progress] i.e. just the bucket of work that it got from [next]
    The behavior of our job is to take items out of progress.remaining and typecheck them.
    It repeats this process until it either runs out of items or its heap grows too big.
    It returns a new progress' {remaining'; completed; deferred} to show the items still
    remaining, the ones it completed, and the ones it had to defer (see below).
    It returns a new typing_result' {errors; deps; telemetry} by merging its own
    newly discovered errors, deps, telemetry onto the (degenerate i.e. empty)
    typing_result it was given as input.
* merge :  (typing_result * progress) (accumulator : typing_result) -> typing_result
    The initial value of accumulator is the same [neutral] that was given to each job.
    After each job, Multi_worker calls [merge] to merge the results of that job
    into the accumulator.
    Our merge function looks at the progress {remaining;completed;deferred} that
    came out of the job, and mutates [files_to_process] by sticking back "remaining+deferred" into it.
    It then merges the typing_result {errors;deps;telemetry} that came out of the job
    with those in its accumulator.

The type signatures for Multi_worker look like they'd allow a variety of implementations,
e.g. having just a single accumulator that starts at "neutral" and feeds one by one into
each job. But we don't do that.

####

The normal computation kind for the type checking service is to type check all symbols in a file.
The type checker would always be able to find a given symbol it needs to perform this job
if all the declarations were computed for all the files in the repo before type checking begins.

However, such eager declaration does not occur in the following scenarios:
  - When the server initialization strategy is Lazy: not having a declaration phase was
      the original meaning of lazy init
  - If the server initialized from a saved state: most decls would not be available because
      saved states only consist of one or more of the following: naming table, dependency
      graph, errors, and hot decls (a small subset of all possible decls)

The decl heap module, when asked for a decl that is not yet in memory, can declare the decl's
file lazily. Declaring a file means parsing the file and extracting the symbols declared in it,
hence the term 'decl'.

This lazy declaration strategy works reasonably well for most cases, but there's a case where
it works poorly: if a particular file that is getting type checked requires a large number
of symbols which have not yet been declared, the unlucky worker ends up serially declaring
all the files that contain those symbols. In some cases, we observe type checking times on
the order of minutes for a single file.

Therefore, to account for the fact that we don't have the overall eager declaration phase
in some initialization scenarios, the decl heap module is now capable of refusing to declare
a file and instead adding the file to a list of deferments.

The type checker worker is then able to return that list and the file which was being type
checked when deferments occurred back to the master process. The master process is then
able to distribute the declaration of the files and the (re)checking of the original
file to all of its workers, thus achieving parallelism and better type checking times overall.

The deferment of declarations is adjustable: it works by using a threshold value that dictates
the maximum number of lazy declarations allowed per file. If the threshold is not set, then
there is no limit, and no declarations would be deferred. If the threshold is set at 1,
then all declarations would be deferred.

The idea behind deferring declarations is similar to the 2nd of the 4 build systems considered in
the following paper (Make, Excel's calc engine, Shake, and Bazel):
    https://www.microsoft.com/en-us/research/uploads/prod/2018/03/build-systems-final.pdf

The paper refers to this approach as "restarting", and further suggests that recording and reusing
the chain of jobs could be used to minimize the number of restarts.
 *)

module Todo : sig
  (** A todo list of workitems. It remembers which workitems
    are currently in progress so that these can be cancelled. *)
  type t

  val create : workitem Big_list.t -> t

  (** [update progress todo] updates [todo] with the [progress] made. *)
  val update : TypingProgress.t -> t -> unit

  (** [consume how_may todo] returns a list of workitems to process next
    if there are any left. [todo] is updated to remember which
    workitems are in progress. *)
  val consume :
    (to_process_count:int -> int) -> t -> workitem list Bucket.bucket

  (** Cancel all workitems currently in progress and return them to the todo list. *)
  val cancel : t -> unit

  val files_checked_count : t -> int
end = struct
  type t = {
    to_process: workitem Big_list.t ref;
    in_progress: workitem Hash_set.Poly.t;
    files_checked_count: int ref;
  }

  let create items =
    {
      to_process = ref items;
      in_progress = Hash_set.Poly.create ();
      files_checked_count = ref 0;
    }

  let update_to_process progress to_process : unit =
    to_process :=
      !to_process
      |> Big_list.append (TypingProgress.remaining progress)
      |> Big_list.append (TypingProgress.deferred progress)

  let update_in_progress progress in_progress =
    (* If workers can steal work from each other, then it's possible that
       some of the files that the current worker completed checking have already
       been removed from the in-progress set. Thus, we should keep track of
       how many type check computations we actually remove from the in-progress
       set. Note that we also skip counting Declare computations,
       since they are not relevant for computing how many files we've type
       checked. *)
    let completed_check_count =
      List.fold
        (TypingProgress.completed progress)
        ~init:0
        ~f:(fun acc workitem ->
          match Hash_set.Poly.strict_remove in_progress workitem with
          | Ok () -> begin
            match workitem with
            | Check _ -> acc + 1
            | _ -> acc
          end
          | _ -> acc)
    in
    completed_check_count

  let update_files_check_count
      progress ~completed_check_count files_checked_count =
    (* Deferred type check computations should be subtracted from completed
       in order to produce an accurate count because they we requeued them, yet
       they were also included in the completed list.
    *)
    let deferred_check_count =
      List.count ~f:Workitem.is_check (TypingProgress.deferred progress)
    in
    let completed_check_count = completed_check_count - deferred_check_count in

    files_checked_count := !files_checked_count + completed_check_count

  let update
      (progress : TypingProgress.t)
      { to_process; in_progress; files_checked_count } : unit =
    update_to_process progress to_process;
    let completed_check_count = update_in_progress progress in_progress in
    update_files_check_count progress ~completed_check_count files_checked_count

  let files_checked_count t = !(t.files_checked_count)

  let consume
      (how_many : to_process_count:int -> int)
      { to_process; in_progress; files_checked_count = _ } =
    if Big_list.is_empty !to_process then
      if Hash_set.Poly.is_empty in_progress then
        Bucket.Done
      else
        Bucket.Wait
    else
      let (next, remaining) =
        Big_list.split_n
          !to_process
          (how_many ~to_process_count:(Big_list.length !to_process))
      in
      to_process := remaining;
      List.iter next ~f:(Hash_set.Poly.add in_progress);
      Bucket.Job next

  let cancel { to_process; in_progress; files_checked_count = _ } =
    to_process :=
      Big_list.append (Hash_set.Poly.to_list in_progress) !to_process
end

type seconds_since_epoch = float

type log_message = string

(** Heartbeat logging: logs when a file takes longer than [heartbeat_interval]
    seconds to type-check. Uses [Timer] (SIGALRM) so it fires during type-checking,
    not after. Only active when [heartbeat_interval] is set. *)
module Heartbeat : sig
  val with_heartbeat : int option -> workitem -> (unit -> 'a) -> 'a
end = struct
  let path_of_workitem workitem =
    match workitem with
    | Check { path; _ }
    | Declare (path, _) ->
      path

  let with_heartbeat interval_seconds workitem fn =
    match interval_seconds with
    | None -> fn ()
    | Some interval ->
      let start_time = Unix.gettimeofday () in
      let prev_cpu_time = ref (Sys.time ()) in
      let prev_counters = ref (Counters.get_counters ()) in
      let timer_id = ref None in
      let rec schedule () =
        timer_id :=
          Some
            (Timer.set_timer
               ~interval:(Float.of_int interval)
               ~callback:(fun () ->
                 let elapsed =
                   Unix.gettimeofday () -. start_time |> Float.to_int
                 in
                 let cpu_time = Sys.time () in
                 let cpu_delta = cpu_time -. !prev_cpu_time in
                 prev_cpu_time := cpu_time;
                 let counters = Counters.get_counters () in
                 let counters_delta =
                   Telemetry.diff ~all:false ~prev:!prev_counters counters
                 in
                 prev_counters := counters;
                 Hh_logger.log
                   "[heartbeat] Still checking %s after %ds (cpu=%.1fs, counters=%s)"
                   (Relative_path.suffix (path_of_workitem workitem))
                   elapsed
                   cpu_delta
                   (Telemetry.to_string counters_delta);
                 Hack_event_logger.client_check_heartbeat
                   ~path:(path_of_workitem workitem)
                   ~start_time;
                 schedule ()))
      in
      schedule ();
      let cancel () = Option.iter !timer_id ~f:Timer.cancel_timer in
      let result =
        try fn () with
        | exn ->
          let e = Exception.wrap exn in
          cancel ();
          Exception.reraise e
      in
      cancel ();
      result
end

let neutral : unit -> typing_result = Typing_service_types.make_typing_result

let should_enable_deferring (file : check_file_workitem) =
  not file.was_already_deferred

(** A [deferment] is a file together with a class decl that we need to fetch
    before we continue with our scheduled typechecking work. *)
type deferment = Relative_path.t * string

type process_file_results = {
  file_diagnostics: Diagnostics.t;
  file_map_reduce_data: Map_reduce.t;
  deferred_decls: deferment list;
}

let scrape_class_names (ast : Nast.program) : S_set.t =
  let names = ref S_set.empty in
  let visitor =
    object
      (* It would look less clumsy to use Aast.reduce, but would use set union which has higher complexity. *)
      inherit [_] Aast.iter as super

      method! on_class_name _ (_p, id) = names := S_set.add id !names

      (* Before naming, explicit class references in expression position
         (new C(), C::m(), C::BAR, C::class, C::m<>, nameof C) are
         [CIexpr (_, _, Id _)]; the [Naming_elab_class_id] pass later rewrites
         them to [CI]. Match them here so this visitor counts the same
         references whether it runs on the raw AST or the NAST. On the NAST
         this branch never fires (no [CIexpr (Id _)] remain), so behavior
         there is unchanged. *)
      method! on_class_id_ env cid =
        match cid with
        | Aast.CIexpr (_, _, Aast.Id (_, id)) -> names := S_set.add id !names
        | _ -> super#on_class_id_ env cid
    end
  in
  visitor#on_program () ast;
  !names

let process_file
    (ctx : Provider_context.t)
    (file : check_file_workitem)
    ~(prefetch_decls_enabled : bool)
    ~(prefetch_decls_threshold : int) : process_file_results =
  let fn = file.path in
  let (file_errors, ast) = Ast_provider.get_ast_with_error ~full:true ctx fn in
  if Diagnostics.has_errors file_errors then
    {
      file_diagnostics = file_errors;
      deferred_decls = [];
      file_map_reduce_data = Map_reduce.empty;
    }
  else begin
    let opts = Provider_context.get_tcopt ctx in
    let ctx = Provider_context.map_tcopt ctx ~f:(fun _tcopt -> opts) in
    (* For decl-heavy files, hand the file's referenced class decls to the
       master as [Declare] workitems rather than folding them all inline in this
       single worker. The master schedules them across worker processes,
       then re-checks this file against the now-warm cache. *)
    let deferred_decls =
      if prefetch_decls_enabled && should_enable_deferring file then begin
        (* Cheap gate: scrape the raw AST (no naming pass) to find decl-heavy
           files. The vast majority of files are below threshold and pay only
           this read-only AST walk instead of a full naming pass. *)
        let num_refs = S_set.cardinal (scrape_class_names ast) in
        if num_refs > prefetch_decls_threshold then begin
          (* Resolve the canonical class names (needs naming) to the files that
             declare them; unresolved refs are dropped, so this list can be
             smaller than [num_refs]. *)
          let deferred =
            Diagnostics.ignore_ (fun () -> Naming.program ctx ast)
            |> scrape_class_names
            |> S_set.elements
            |> List.filter_map ~f:(fun class_name ->
                   Naming_provider.get_class_path ctx class_name >>| fun fn ->
                   (fn, class_name))
          in
          Hh_logger.log
            "[defer decls] %s: deferring %d decls to prefetch before type checking"
            (Relative_path.suffix fn)
            (List.length deferred);
          deferred
        end else
          []
      end else
        []
    in
    match deferred_decls with
    | _ :: _ ->
      {
        file_diagnostics = Diagnostics.empty;
        deferred_decls;
        file_map_reduce_data = Map_reduce.empty;
      }
    | [] -> begin
      try
        let (file_errors, tasts) =
          Typing_check_job.calc_errors_and_tast
            ctx
            fn
            ~drop_fixmed:false
            ~full_ast:ast
        in
        {
          file_diagnostics = file_errors;
          deferred_decls = [];
          file_map_reduce_data = Map_reduce.map ctx fn tasts file_errors;
        }
      with
      | Worker_cancel.Worker_should_exit as exn ->
        (* Cancellation requests must be re-raised *)
        let e = Exception.wrap exn in
        Exception.reraise e
      | exn ->
        let e = Exception.wrap exn in
        prerr_endline ("Exception on file " ^ Relative_path.S.to_string fn);
        Exception.reraise e
    end
  end

module ProcessFilesTally = struct
  (** Counters for the [check_file_workitem] of each sort being processed *)
  type t = {
    decls: int;  (** how many [Declare] items we performed *)
    checks_done: int;  (** how many [Check] items we typechecked *)
    checks_deferred: int;  (** how many [Check] items we deferred to later *)
    decls_deferred: int;  (** how many [Declare] items we added for later *)
    exceeded_cap_count: int;  (** how many times we exceeded the memory cap *)
  }

  let empty =
    {
      decls = 0;
      checks_done = 0;
      checks_deferred = 0;
      decls_deferred = 0;
      exceeded_cap_count = 0;
    }

  let incr_decls tally = { tally with decls = tally.decls + 1 }

  let record_caps ~workitem_ends_under_cap tally =
    if workitem_ends_under_cap then
      tally
    else
      { tally with exceeded_cap_count = tally.exceeded_cap_count + 1 }

  let incr_checks tally deferred_decls =
    if List.is_empty deferred_decls then
      { tally with checks_done = tally.checks_done + 1 }
    else
      {
        tally with
        checks_deferred = tally.checks_deferred + 1;
        decls_deferred = tally.decls_deferred + List.length deferred_decls;
      }

  let count tally = tally.checks_done + tally.checks_deferred + tally.decls

  let get_telemetry tally =
    Telemetry.create ()
    |> Telemetry.int_ ~key:"decls" ~value:tally.decls
    |> Telemetry.int_ ~key:"checks_done" ~value:tally.checks_done
    |> Telemetry.int_ ~key:"checks_deferred" ~value:tally.checks_deferred
    |> Telemetry.int_ ~key:"decls_deferred" ~value:tally.decls_deferred
    |> Telemetry.int_ ~key:"exceeded_cap_count" ~value:tally.exceeded_cap_count
end

let get_stats ~include_slightly_costly_stats tally :
    Hack_event_logger.ProfileTypeCheck.stats =
  let telemetry =
    Counters.get_counters ()
    |> Telemetry.object_
         ~key:"tally"
         ~value:(ProcessFilesTally.get_telemetry tally)
  in
  Hack_event_logger.ProfileTypeCheck.get_stats
    ~include_current_process:true
    ~include_slightly_costly_stats
    ~shmem_heap_size:(Shared_mem.SMTelemetry.heap_size ())
    telemetry

external hh_malloc_trim : unit -> unit = "hh_malloc_trim"

type workitem_accumulator = {
  diagnostics: Diagnostics.t;
  map_reduce_data: Map_reduce.t;
  tally: ProcessFilesTally.t;
  stats: Hack_event_logger.ProfileTypeCheck.stats;
}

let invoke_testing_hook_after_file_check hooks ~deferred_workitems file =
  (* A deferred check queues declaration work and a retry of this file, so it
     has not completed and must not invoke the completion hook. *)
  if List.is_empty deferred_workitems then
    Option.iter hooks ~f:(fun hooks -> hooks.Server_env.after_file_check file)

let process_one_workitem
    ~ctx
    ~(check_info : check_info)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~batch_info
    ~memory_cap
    ~longlived_workers
    (fn : workitem)
    ({ diagnostics; map_reduce_data; tally; stats } : workitem_accumulator) :
    TypingProgress.progress_outcome * workitem_accumulator =
  let heartbeat_interval = check_info.heartbeat_interval in
  let workitem_cap_mb = Option.value memory_cap ~default:Int.max_value in
  let type_check_twice =
    check_info.per_file_profiling
      .Hack_event_logger.PerFileProfilingConfig.profile_type_check_twice
  in

  let ( file,
        decl,
        mid_stats,
        file_diagnostics,
        map_reduce_data,
        deferred_workitems,
        tally ) =
    Heartbeat.with_heartbeat heartbeat_interval fn @@ fun () ->
    match fn with
    | Check file ->
      let { file_diagnostics; file_map_reduce_data; deferred_decls } =
        process_file
          ctx
          file
          ~prefetch_decls_enabled:check_info.prefetch_decls_enabled
          ~prefetch_decls_threshold:check_info.prefetch_decls_threshold
      in
      let map_reduce_data =
        Map_reduce.reduce map_reduce_data file_map_reduce_data
      in
      let mid_stats =
        if type_check_twice then
          Some (get_stats ~include_slightly_costly_stats:false tally)
        else
          None
      in
      begin
        if type_check_twice then
          let (_ignored : process_file_results) =
            process_file
              ctx
              file
              ~prefetch_decls_enabled:false
              ~prefetch_decls_threshold:0
          in
          ()
      end;
      let tally = ProcessFilesTally.incr_checks tally deferred_decls in
      let deferred =
        if List.is_empty deferred_decls then
          []
        else
          List.map deferred_decls ~f:(fun fn -> Declare fn)
          @ [Check { file with was_already_deferred = true }]
      in
      ( Some file,
        None,
        mid_stats,
        file_diagnostics,
        map_reduce_data,
        deferred,
        tally )
    | Declare (_path, class_name) ->
      let (_ : Decl_provider.class_decl Decl_entry.t) =
        Decl_provider.get_class ctx class_name
        (* I assume we don't have to add dependency edges for decling *)
        [@@alert "-dependencies"]
      in
      ( None,
        Some class_name,
        None,
        Diagnostics.empty,
        map_reduce_data,
        [],
        ProcessFilesTally.incr_decls tally )
  in
  let diagnostics = Diagnostics.merge file_diagnostics diagnostics in
  let workitem_ends_under_cap = Gc_utils.get_heap_size () <= workitem_cap_mb in
  let final_stats =
    get_stats
      ~include_slightly_costly_stats:
        ((not longlived_workers) && not workitem_ends_under_cap)
      tally
  in
  let (workitem_end_stats, workitem_end_second_stats) =
    match mid_stats with
    | None -> (final_stats, None)
    | Some mid_stats -> (mid_stats, Some final_stats)
  in

  (* If the major heap has exceeded the bounds, we (1) first try and bring the size back down
     by flushing the parser cache and doing a major GC; (2) if this fails, we decline to typecheck
     the remaining files.
     We use [quick_stat] instead of [stat] in Gc_utils.get_heap_size in order to avoid walking the major heap,
     and we don't change the minor heap because it's small and fixed-size.
     This test is performed after we've processed at least one item, to ensure we make at least some progress. *)
  let tally = ProcessFilesTally.record_caps ~workitem_ends_under_cap tally in
  let workitem_ends_under_cap =
    if workitem_ends_under_cap || not longlived_workers then
      workitem_ends_under_cap
    else begin
      Shared_mem.invalidate_local_caches ();
      Hack_event_logger.flush ();
      Gc.compact ();
      hh_malloc_trim ();
      Gc_utils.get_heap_size () <= workitem_cap_mb
    end
  in

  (match fn with
  | Declare _ -> ()
  | Check checked_file ->
    Hack_event_logger.ProfileTypeCheck.process_workitem
      ~batch_info
      ~workitem_index:(ProcessFilesTally.count tally)
      ~file:(Option.map file ~f:(fun file -> file.path))
      ~file_was_already_deferred:
        (Option.map file ~f:(fun file -> file.was_already_deferred))
      ~decl
      ~error_code:(Diagnostics.choose_code_opt file_diagnostics)
      ~workitem_ends_under_cap
      ~workitem_start_stats:stats
      ~workitem_end_stats
      ~workitem_end_second_stats;
    invoke_testing_hook_after_file_check
      type_check_testing_hooks
      ~deferred_workitems
      checked_file.path);

  ( { TypingProgress.deferred_workitems; continue = workitem_ends_under_cap },
    { diagnostics; map_reduce_data; tally; stats = final_stats } )

let process_workitems
    (ctx : Provider_context.t)
    ({ diagnostics; map_reduce_data; dep_edges; profiling_info } :
      typing_result)
    (progress : TypingProgress.t)
    ~(memory_cap : int option)
    ~(longlived_workers : bool)
    ~(check_info : check_info)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~(worker_id : string)
    ~(batch_number : int)
    ~(typecheck_info : Hack_event_logger.ProfileTypeCheck.typecheck_info) :
    typing_result * TypingProgress.t =
  Decl_counters.set_mode
    check_info.per_file_profiling
      .Hack_event_logger.PerFileProfilingConfig.profile_decling;
  let _prev_counters_state = Counters.reset () in
  let batch_info =
    Hack_event_logger.ProfileTypeCheck.get_batch_info
      ~typecheck_info
      ~worker_id
      ~batch_number
      ~batch_size:(List.length (TypingProgress.remaining progress))
      ~start_batch_stats:
        (get_stats ~include_slightly_costly_stats:true ProcessFilesTally.empty)
  in

  if not longlived_workers then Shared_mem.invalidate_local_caches ();
  File_provider.local_changes_push_sharedmem_stack ();
  Ast_provider.local_changes_push_sharedmem_stack ();

  (* Process as many files as we can, and merge in their diagnostics *)
  let (progress, { diagnostics; map_reduce_data; tally = _; stats = _ }) =
    let init =
      {
        diagnostics;
        map_reduce_data;
        tally = ProcessFilesTally.empty;
        stats =
          get_stats ~include_slightly_costly_stats:true ProcessFilesTally.empty;
      }
    in
    TypingProgress.progress_through ~init progress
    @@ process_one_workitem
         ~ctx
         ~check_info
         ~type_check_testing_hooks
         ~batch_info
         ~memory_cap
         ~longlived_workers
  in

  (* Update edges *)
  let new_dep_edges =
    Typing_deps.flush_ideps_batch (Provider_context.get_deps_mode ctx)
  in
  let dep_edges = Typing_deps.merge_dep_edges dep_edges new_dep_edges in
  if
    Provider_context.get_tcopt ctx
    |> Typechecker_options.record_fine_grained_dependencies
  then
    Typing_pessimisation_deps.finalize (Provider_context.get_deps_mode ctx);

  (* Gather up our various forms of telemetry... *)
  let end_heap_mb = Gc.((quick_stat ()).Stat.heap_words) * 8 / 1024 / 1024 in
  let this_batch_profiling_info =
    Telemetry.create ()
    |> Telemetry.object_ ~key:"operations" ~value:(Counters.get_counters ())
    |> Telemetry.int_ ~key:"end_heap_mb_sum" ~value:end_heap_mb
    |> Telemetry.int_ ~key:"batch_count" ~value:1
  in
  let profiling_info = Telemetry.add profiling_info this_batch_profiling_info in

  Typing_logger.flush_buffers ();
  Ast_provider.local_changes_pop_sharedmem_stack ();
  File_provider.local_changes_pop_sharedmem_stack ();
  ({ diagnostics; map_reduce_data; dep_edges; profiling_info }, progress)

let load_and_process_workitems
    (ctx : Provider_context.t)
    (typing_result : typing_result)
    (progress : TypingProgress.t)
    ~(memory_cap : int option)
    ~(longlived_workers : bool)
    ~(check_info : check_info)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~(worker_id : string)
    ~(batch_number : int)
    ~(typecheck_info : Hack_event_logger.ProfileTypeCheck.typecheck_info) :
    typing_result * TypingProgress.t =
  Option.iter check_info.memtrace_dir ~f:(fun temp_dir ->
      let file =
        Stdlib.Filename.temp_file ~temp_dir "memtrace.worker." ".ctf"
      in
      Daemon.start_memtracing file);
  (* When the type-checking worker receives SIGUSR1, display a position which
     corresponds approximately with the function/expression being checked. *)
  Sys_utils.set_signal
    Sys.sigusr1
    (Sys.Signal_handle Typing.debug_print_last_pos);
  process_workitems
    ctx
    typing_result
    progress
    ~memory_cap
    ~longlived_workers
    ~check_info
    ~type_check_testing_hooks
    ~worker_id
    ~batch_number
    ~typecheck_info

(*****************************************************************************)
(* Let's go! That's where the action is *)
(*****************************************************************************)

module Counts = struct
  type t = int S_map.t

  let increment map key =
    let prev_count = S_map.find_opt key map |> Option.value ~default:0 in
    S_map.add key (prev_count + 1) map
end

module ErrorStats = struct
  type t = {
    total_error_count: int;
    time_first_error: seconds_since_epoch option;
  }

  let empty = { total_error_count = 0; time_first_error = None }

  let update errors { total_error_count; time_first_error } : t =
    {
      total_error_count = total_error_count + Diagnostics.count errors;
      time_first_error =
        (match time_first_error with
        | Some t -> Some t
        | None ->
          if Diagnostics.has_errors errors then
            Some (Unix.gettimeofday ())
          else
            None);
    }
end

(** If [discard_warnings], [add_warnings_to_ss mergebase_warning_hashes errors ~discard_warnings]
  adds the hashes of the warnings in `error` to `mergebase_warning_hashes`. *)
let add_warnings_to_ss mergebase_warning_hashes errors ~discard_warnings :
    Warnings_saved_state.t option =
  Option.map mergebase_warning_hashes ~f:(fun mergebase_warning_hashes ->
      if discard_warnings then
        let warning_hashes = Diagnostics.make_warning_saved_state errors in
        Warnings_saved_state.union mergebase_warning_hashes warning_hashes
      else
        mergebase_warning_hashes)

let filter_out_warnings mergebase_warning_hashes errors ~discard_warnings =
  let mergebase_warning_hashes =
    add_warnings_to_ss mergebase_warning_hashes errors ~discard_warnings
  in
  let errors =
    Diagnostics.filter_out_mergebase_warnings mergebase_warning_hashes errors
  in
  (mergebase_warning_hashes, errors)

module Merge : sig
  val merge :
    batch_counts_by_worker_id:Counts.t ref ->
    error_stats:ErrorStats.t ref ->
    check_info:check_info ->
    Todo.t ->
    int ->
    log_message * typing_result * TypingProgress.t ->
    Warnings_saved_state.t option * typing_result ->
    Warnings_saved_state.t option * typing_result
end = struct
  let process_errors errors error_stats ~stream_errors ~log_errors : unit =
    error_stats := ErrorStats.update errors !error_stats;
    if log_errors then (
      let error_count = Diagnostics.count errors in
      if error_count > 0 then (
        let max_errors = 5 in
        Hh_logger.log
          "%d errors in batch. Showing first %d:"
          error_count
          max_errors;
        List.iter
          (List.take (Diagnostics.get_diagnostic_list errors) max_errors)
          ~f:(fun error ->
            let { User_diagnostic.severity; claim; code; _ } = error in
            let (pos, msg) = claim in
            let (l1, l2, c1, c2) = Pos.info_pos_extended pos in
            Hh_logger.log
              "%s: %s(%d:%d-%d:%d) [%d] %s"
              (User_diagnostic.Severity.to_all_caps_string severity)
              (Relative_path.suffix @@ Pos.filename pos)
              l1
              c1
              l2
              c2
              code
              msg)
      );
      (* Handle errors paradigm (3) - push updates to errors-file as soon as their batch is finished *)
      if stream_errors then Server_progress.ErrorsWrite.report errors
    )

  let process_and_merge_typing_results
      (produced_by_job : Typing_service_types.typing_result)
      ~(acc : Typing_service_types.typing_result)
      (mergebase_warning_hashes : Warnings_saved_state.t option)
      ~(stream_errors : bool)
      ~log_errors
      ~discard_warnings
      (error_stats : ErrorStats.t ref) : _ * Typing_service_types.typing_result
      =
    let (mergebase_warning_hashes, diagnostics) =
      filter_out_warnings
        mergebase_warning_hashes
        produced_by_job.diagnostics
        ~discard_warnings
    in
    let produced_by_job = { produced_by_job with diagnostics } in
    process_errors
      produced_by_job.diagnostics
      error_stats
      ~stream_errors
      ~log_errors;

    Typing_deps.register_discovered_dep_edges produced_by_job.dep_edges;
    Typing_deps.register_discovered_dep_edges acc.dep_edges;

    let produced_by_job =
      { produced_by_job with dep_edges = Typing_deps.dep_edges_make () }
    in
    let acc = { acc with dep_edges = Typing_deps.dep_edges_make () } in

    let acc = accumulate_job_output produced_by_job acc in
    (mergebase_warning_hashes, acc)

  (** Merge the results from multiple workers.

    We don't really care about which files are left unchecked since we use
    (gasp) mutation to track that, so combine the errors but always return an
    empty list for the list of unchecked files. *)
  let merge
      ~(batch_counts_by_worker_id : Counts.t ref)
      ~(error_stats : ErrorStats.t ref)
      ~(check_info : check_info)
      (todo : Todo.t)
      (workitems_initial_count : int)
      ( (worker_id : string),
        (produced_by_job : Typing_service_types.typing_result),
        (progress : TypingProgress.t) )
      ((warnings_saved_state, acc) :
        Warnings_saved_state.t option * Typing_service_types.typing_result) :
      Warnings_saved_state.t option * Typing_service_types.typing_result =
    batch_counts_by_worker_id :=
      Counts.increment !batch_counts_by_worker_id worker_id;

    Todo.update progress todo;

    Server_progress.write_percentage
      ~operation:"typechecking"
      ~done_count:(Todo.files_checked_count todo)
      ~total_count:workitems_initial_count
      ~unit:"files"
      ~extra:None;

    process_and_merge_typing_results
      produced_by_job
      ~acc
      warnings_saved_state
      ~stream_errors:check_info.log_errors
      ~log_errors:check_info.log_errors
      ~discard_warnings:check_info.discard_warnings
      error_stats
end

let next
    (workers : Multi_worker.worker list option)
    (todo : Todo.t)
    (record : Measure.record) : unit -> TypingProgress.t Bucket.bucket =
  let num_workers =
    match workers with
    | Some w -> List.length w
    | None -> 1
  in
  fun () ->
    Measure.time ~record "time" @@ fun () ->
    match num_workers with
    | 0 ->
      (* When num_workers is zero, the execution mode is delegate-only, so we give an empty bucket to Multi_worker for execution. *)
      Bucket.Job (TypingProgress.init [])
    | _ ->
      Todo.consume
        (fun ~to_process_count ->
          Bucket.calculate_bucket_size
            ~num_jobs:to_process_count
            ~num_workers
            ())
        todo
      |> Bucket.map ~f:TypingProgress.init

let on_cancelled (next : unit -> 'a Bucket.bucket) (todo : Todo.t) :
    unit -> 'a list =
 fun () ->
  Todo.cancel todo;
  let rec add_next acc =
    match next () with
    | Bucket.Job j -> add_next (j :: acc)
    | Bucket.Wait
    | Bucket.Done ->
      acc
  in
  add_next []

let rec drain_events
    warnings_saved_state (done_count, total_count, handle, check_info) :
    (Warnings_saved_state.t option * (int * int), _) result =
  match Hh_distc_ffi.recv handle with
  | Ok (Some (Hh_distc_types.Errors errors)) ->
    let (warnings_saved_state, errors) =
      filter_out_warnings
        warnings_saved_state
        errors
        ~discard_warnings:check_info.discard_warnings
    in
    if check_info.log_errors then Server_progress.ErrorsWrite.report errors;
    drain_events
      warnings_saved_state
      (done_count, total_count, handle, check_info)
  | Ok (Some (Hh_distc_types.TypingStart total_count)) ->
    drain_events
      warnings_saved_state
      (done_count, total_count, handle, check_info)
  | Ok (Some (Hh_distc_types.TypingProgress n)) ->
    let done_count = done_count + n in
    drain_events
      warnings_saved_state
      (done_count, total_count, handle, check_info)
  | Ok None -> Ok (warnings_saved_state, (done_count, total_count))
  | Error error -> Error error

type distc_config_options = {
  enable_fanout_aware_distc: bool;
  fanout_threshold: int;
  fanout_full_init_threshold: int;
}

type distc_config = distc_config_options option

type 'env distc_outcome =
  | Success of
      Diagnostics.t
      * Map_reduce.t
      * Typing_deps.dep_edges
      * Warnings_saved_state.t option
      * 'env
  | DistCError of log_message
  | Cancel of 'env * Multi_threaded_call.cancel_reason

(**
  This is the event loop that powers hh_distc. It keeps looping and calling
  select on a series of fds including ones from watchman, hh_distc, hh_client, etc.

  When one of the fds activate, the loops continues one iteration and decides what to
  do.

  Most of the time the main fd that will be selected is the hh_distc one as it reports
  progress events on a very frequent interval. We can then use these progress events
  to stream errors and report progress back to the user.
*)
let rec event_loop
    ~(done_count : int)
    ~(total_count : int)
    ~(interrupt : 'env Multi_threaded_call.interrupt_config)
    ~(handlers :
       (Unix.file_descr * 'env Multi_threaded_call.interrupt_handler) list)
    ~(fd_distc : Unix.file_descr)
    ~(handle : Hh_distc_ffi.handle)
    ~(check_info : check_info)
    ~(hhdg_path : string)
    ~warnings_saved_state : _ distc_outcome =
  let handler_fds = List.map handlers ~f:fst in
  (* hh_distc sends a byte each time new events are ready. *)
  let ready_fds =
    Poll.ready_fds_read_non_interrupted
      (fd_distc :: handler_fds)
      ~timeout_ms:None
  in
  if List.mem ~equal:Poly.( = ) ready_fds fd_distc then
    match Sys_utils.read_non_intr fd_distc 1 with
    | None ->
      Server_progress.write "hh_distc done";
      (match Hh_distc_ffi.join handle with
      | Ok (errors, map_reduce_data) ->
        let (warnings_saved_state, errors) =
          filter_out_warnings
            warnings_saved_state
            errors
            ~discard_warnings:check_info.discard_warnings
        in
        (* TODO: Clear in memory deps. Doesn't effect correctness but can cause larger fanouts *)
        Typing_deps.replace (Typing_deps_mode.InMemoryMode (Some hhdg_path));
        Success
          ( errors,
            Map_reduce.of_ffi map_reduce_data,
            Typing_deps.dep_edges_make (),
            warnings_saved_state,
            interrupt.Multi_threaded_call.env )
      | Error error -> DistCError error)
    | Some _ ->
      (match
         drain_events
           warnings_saved_state
           (done_count, total_count, handle, check_info)
       with
      | Ok (warnings_saved_state, (done_count, total_count)) ->
        Server_progress.write_percentage
          ~operation:"hh_distc checking"
          ~done_count
          ~total_count
          ~unit:"files"
          ~extra:None;
        event_loop
          ~done_count
          ~total_count
          ~interrupt
          ~handlers
          ~fd_distc
          ~handle
          ~check_info
          ~hhdg_path
          ~warnings_saved_state
      | Error error -> DistCError error)
  else
    let (env, decision, handlers) =
      List.fold
        handlers
        ~init:
          ( interrupt.Multi_threaded_call.env,
            Multi_threaded_call.Continue,
            handlers )
        ~f:(fun (env, decision, handlers) (fd, handler) ->
          match (decision, not @@ List.mem ~equal:Poly.( = ) ready_fds fd) with
          | (_, false) ->
            (* skip handlers whose fd isn't ready *)
            (env, decision, handlers)
          | (Multi_threaded_call.Cancel _, _) ->
            (* if a previous handler has decided to cancel, skip further handlers *)
            (env, decision, handlers)
          | (Multi_threaded_call.Continue, true) ->
            let (env, decision) = handler env in
            (* running a handler could have changed the handlers,
               * so need to regenerate them based on new environment *)
            let handlers =
              interrupt.Multi_threaded_call.handlers
                interrupt.Multi_threaded_call.env
            in
            (env, decision, handlers))
    in
    let interrupt = { interrupt with Multi_threaded_call.env } in
    match decision with
    | Multi_threaded_call.Cancel reason ->
      let () = Hh_distc_ffi.cancel handle in
      Cancel (interrupt.Multi_threaded_call.env, reason)
    | Multi_threaded_call.Continue ->
      event_loop
        ~done_count
        ~total_count
        ~interrupt
        ~handlers
        ~fd_distc
        ~handle
        ~check_info
        ~hhdg_path
        ~warnings_saved_state

(** Phase in which hh_distc failed. Used to tag log messages so Scuba queries
    can sub-classify failures without parsing the underlying RE error string. *)
type distc_failure_phase =
  | Spawn
  | Runtime

let string_of_distc_failure_phase = function
  | Spawn -> "spawn"
  | Runtime -> "runtime"

(** Log a hh_distc error and exit with the typed Distc_failed status.
    [phase] identifies where the failure happened; it is prefixed onto the
    logged message so Scuba queries can sub-classify failures by phase.
    The full error message is preserved in logs and HackEventLogger so it can
    be further sub-classified (e.g. RESOURCE_EXHAUSTED, USER_QUOTA) via Scuba. *)
let exit_distc_failed ~(phase : distc_failure_phase) (msg : string) : 'a =
  let tagged =
    Printf.sprintf "[%s] %s" (string_of_distc_failure_phase phase) msg
  in
  Hh_logger.log "Error with hh_distc: %s" tagged;
  Hack_event_logger.invariant_violation_bug "hh_distc failure" ~data:tagged;
  raise Exit_status.(Exit_with Distc_failed)

(**
  This is the main process function that triggers a full init via hh_distc.

  We FFI into rustland to activate hh_distc to get back an opaque handle. We
  can then use this handle to poll for progress and eventually join the handle
  to get all the errors we need to return to the user.

  This also works with the existing hh_server incrementality paradigm because
  after each typecheck, we generate a full dep graph, which we then use to replace
  the existing hh_server dep graph.

  We return a result where Ok represents a completed typecheck and Error represents
  a cancelled typecheck. Any errors from hh_distc are considered fatal and result in
  a clean exit with `Exit_status.Distc_failed`, which terminates hh_server with exit
  code 229.
*)
let process_with_hh_distc
    ~(root : Path.t option)
    ~(fanout : Typing_service_types.workitem Big_list.t option)
    ~(interrupt : 'a Multi_threaded_call.interrupt_config)
    ~(check_info : check_info)
    ~(tcopt : Typechecker_options.t)
    ~warnings_saved_state : _ distc_outcome =
  (* We don't want to use with_tempdir because we need to keep the folder around
     for subseqent typechecks that will read the dep graph in the folder *)
  let root = Option.value_exn root in
  let ss_dir = Tempfile.mkdtemp ~skip_mocking:false in
  let hhdg_path =
    Path.(to_string @@ concat ss_dir "hh_mini_saved_state.hhdg")
  in
  let fanout =
    match fanout with
    | None -> None
    | Some fanout ->
      Some
        (List.filter_map (Big_list.as_list fanout) ~f:(fun wi ->
             match wi with
             | Check { path; _ } -> Some (Relative_path.suffix path)
             | _ -> None))
  in
  let hh_distc_handle =
    match
      Hh_distc_ffi.spawn
        ~root:(Path.to_string root)
        ~ss_dir:(Path.to_string ss_dir)
        ~hhdg_path
        ~fanout
        tcopt
    with
    | Ok h -> h
    | Error msg -> exit_distc_failed ~phase:Spawn msg
  in
  let re_session_id = Hh_distc_ffi.get_re_session_id hh_distc_handle in
  Hh_logger.log "hh_distc RE session id: %s" re_session_id;
  Server_progress.write "hh_distc running";
  event_loop
    ~done_count:0
    ~total_count:0
    ~interrupt
    ~handlers:
      (interrupt.Multi_threaded_call.handlers interrupt.Multi_threaded_call.env)
    ~fd_distc:(Hh_distc_ffi.get_fd hh_distc_handle)
    ~handle:hh_distc_handle
    ~check_info
    ~hhdg_path
    ~warnings_saved_state

(**
  `next` and `merge` both run in the master process and update mutable
  state in order to track work in progress and work remaining.
  `job` runs in each worker and does not have access to this mutable state.
 *)
let process_in_parallel
    (ctx : Provider_context.t)
    (workers : Multi_worker.worker list option)
    (telemetry : Telemetry.t)
    (workitems : workitem Big_list.t)
    ~(interrupt : 'a Multi_threaded_call.interrupt_config)
    ~(memory_cap : int option)
    ~(longlived_workers : bool)
    ~(check_info : check_info)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~warnings_saved_state
    ~(typecheck_info : Hack_event_logger.ProfileTypeCheck.typecheck_info) :
    _
    * typing_result
    * Telemetry.t
    * _
    * (Relative_path.t list * Multi_threaded_call.cancel_reason) option
    * seconds_since_epoch option =
  let record = Measure.create () in
  (* [record] is used by [next] *)
  let todo = Todo.create workitems in
  let workitems_initial_count = Big_list.length workitems in
  let error_stats = ref ErrorStats.empty in
  let batch_counts_by_worker_id = ref S_map.empty in

  let next = next workers todo record in
  (* The [job] lambda is marshalled, sent to the worker process, unmarshalled there, and executed.
     It is marshalled immediately before being executed. *)
  let job
      ((_warnings_saved_state, typing_result) : _ * typing_result)
      (progress : TypingProgress.t) : string * typing_result * TypingProgress.t
      =
    let worker_id = Option.value ~default:"main" (Hh_logger.get_id ()) in
    let (typing_result, computation_progress) =
      load_and_process_workitems
        ctx
        ~memory_cap
        ~longlived_workers
        ~check_info
        ~type_check_testing_hooks
        ~typecheck_info
        ~worker_id
        ~batch_number:
          (S_map.find_opt worker_id !batch_counts_by_worker_id
          |> Option.value ~default:0)
        typing_result
        progress
    in
    (worker_id, typing_result, computation_progress)
  in
  let ((warnings_saved_state, typing_result), env, cancelled_results) =
    Multi_worker.call_with_interrupt
      workers
      ~job
      ~neutral:(warnings_saved_state, neutral ())
      ~merge:
        (Merge.merge
           ~batch_counts_by_worker_id
           ~error_stats
           ~check_info
           todo
           workitems_initial_count)
      ~next
      ~on_cancelled:(on_cancelled next todo)
      ~interrupt
  in
  let paths_of (unfinished : TypingProgress.t list) : Relative_path.t list =
    let paths_of (cancelled_progress : TypingProgress.t) =
      let cancelled_computations =
        TypingProgress.remaining cancelled_progress
      in
      let paths_of paths (cancelled_workitem : workitem) =
        match cancelled_workitem with
        | Check { path; _ } -> path :: paths
        | _ -> paths
      in
      List.fold cancelled_computations ~init:[] ~f:paths_of
    in
    List.concat (List.map unfinished ~f:paths_of)
  in
  let cancelled_results =
    Option.map cancelled_results ~f:(fun (unfinished, reason) ->
        (paths_of unfinished, reason))
  in
  ( warnings_saved_state,
    typing_result,
    telemetry,
    env,
    cancelled_results,
    !error_stats.ErrorStats.time_first_error )

type 'a job_result =
  'a * (Relative_path.t list * Multi_threaded_call.cancel_reason) option

module type Mocking_sig = sig
  val with_test_mocking :
    (* real job payload, that we can modify... *)
    workitem Big_list.t ->
    ((* ... before passing it to the real job executor... *)
     workitem Big_list.t ->
    'a job_result) ->
    (* ... which output we can also modify. *)
    'a job_result
end

module NoMocking = struct
  let with_test_mocking fnl f = f fnl
end

module TestMocking = struct
  let cancelled = ref Relative_path.Set.empty

  let set_is_cancelled x = cancelled := Relative_path.Set.add !cancelled x

  let is_cancelled x = Relative_path.Set.mem !cancelled x

  let with_test_mocking
      (fnl : workitem Big_list.t) (f : workitem Big_list.t -> 'a job_result) :
      'a job_result =
    let (mock_cancelled, fnl) =
      List.partition_map (Big_list.as_list fnl) ~f:(fun computation ->
          match computation with
          | Check { path; _ } ->
            if is_cancelled path then
              First path
            else
              Second computation
          | _ -> Second computation)
    in
    (* Only cancel once to avoid infinite loops *)
    cancelled := Relative_path.Set.empty;
    let (res, unfinished_and_reason) = f (Big_list.create fnl) in
    let unfinished_and_reason =
      match unfinished_and_reason with
      | None when List.is_empty mock_cancelled -> None
      | None ->
        Some
          ( mock_cancelled,
            {
              Multi_threaded_call.user_message = "mock cancel";
              log_message = "mock cancel";
              timestamp = 0.0;
            } )
      | Some (cancelled, reason) -> Some (mock_cancelled @ cancelled, reason)
    in
    (res, unfinished_and_reason)
end

module Mocking =
  (val if
         Injector_config.use_test_stubbing
         && not (Injector_config.Exclusions.exclude_typing_check_service ())
       then
         (module TestMocking : Mocking_sig)
       else
         (module NoMocking : Mocking_sig))

type result = {
  diagnostics: Diagnostics.t;
  warnings_saved_state: Warnings_saved_state.t option;
  telemetry: Telemetry.t;
  time_first_error: seconds_since_epoch option;
}

let invoke_testing_hook_before_file_checks hooks ~workers ~hh_distc_config files
    =
  if
    Option.is_some hooks
    && (Option.is_some workers || Option.is_some hh_distc_config)
  then
    failwith
      "Typechecking test hooks require workers=None and hh_distc_config=None";
  Option.iter hooks ~f:(fun hooks -> hooks.Server_env.before_file_checks files)

let go_with_interrupt
    (ctx : Provider_context.t)
    (workers : Multi_worker.worker list option)
    (telemetry : Telemetry.t)
    (fnl : Relative_path.t list)
    ~(root : Path.t option)
    ~(interrupt : 'a Multi_threaded_call.interrupt_config)
    ~(longlived_workers : bool)
    ~(hh_distc_config : distc_config)
    ~(check_info : check_info)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~warnings_saved_state : (_ * result) job_result =
  invoke_testing_hook_before_file_checks
    type_check_testing_hooks
    ~workers
    ~hh_distc_config
    fnl;
  let typecheck_info =
    Hack_event_logger.ProfileTypeCheck.get_typecheck_info
      ~init_id:check_info.init_id
      ~check_reason:check_info.check_reason
      ~recheck_id:check_info.recheck_id
      ~start_hh_stats:(Cgroup_profiler.get_initial_stats ())
      ~start_typecheck_stats:
        (Hack_event_logger.ProfileTypeCheck.get_stats
           ~include_current_process:false
           ~include_slightly_costly_stats:true
           ~shmem_heap_size:(Shared_mem.SMTelemetry.heap_size ())
           (Telemetry.create ()))
      ~config:check_info.per_file_profiling
  in
  let tcopt = Provider_context.get_tcopt ctx in
  let sample_rate = Typechecker_options.typecheck_sample_rate tcopt in
  let original_fnl = fnl in
  let fnl = Big_list.create fnl in
  Server_progress.write "typechecking %d files" (Big_list.length fnl);
  let fnl =
    if Float.(sample_rate >= 1.0) then
      fnl
    else
      let result =
        Big_list.filter ~f:(Find_utils.sample_filter ~sample_rate) fnl
      in
      Hh_logger.log
        "Sampling %f percent of files: %d out of %d"
        sample_rate
        (Big_list.length result)
        (Big_list.length fnl);
      result
  in
  let fnl =
    Big_list.map fnl ~f:(fun path ->
        Check { path; was_already_deferred = false })
  in
  let num_workers = Typechecker_options.num_local_workers tcopt in
  let workers =
    match (workers, num_workers) with
    | (Some workers, Some num_local_workers) ->
      let (workers, _) = List.split_n workers num_local_workers in
      Some workers
    | (None, _)
    | (_, None) ->
      workers
  in
  Mocking.with_test_mocking fnl @@ fun fnl ->
  let ( warnings_saved_state,
        typing_result,
        telemetry,
        env,
        cancelled_fnl_and_reason,
        time_first_error ) =
    let fanout_size = Big_list.length fnl in
    let (will_use_distc, fanout_aware_distc) =
      match hh_distc_config with
      | Some distc_config ->
        let fanout_aware_distc =
          fanout_size >= distc_config.fanout_threshold
          && fanout_size < distc_config.fanout_full_init_threshold
          && distc_config.enable_fanout_aware_distc
        in
        (fanout_size >= distc_config.fanout_threshold, fanout_aware_distc)
      | None -> (false, false)
    in
    let fanout =
      if fanout_aware_distc then (
        Hh_logger.log
          "hh_distc performing fanout-aware typechecking. Fanout of size: %d"
          fanout_size;
        Some fnl
      ) else
        None
    in
    if check_info.log_errors then
      Server_progress.ErrorsWrite.telemetry
        (Telemetry.create ()
        |> Telemetry.bool_ ~key:"will_use_distc" ~value:will_use_distc);
    if will_use_distc then
      (* TODO(ljw): time_first_error isn't properly calculated in this path *)
      (* distc doesn't yet give any profiling_info about how its workers fared *)
      let profiling_info = Telemetry.create () in
      match
        process_with_hh_distc
          ~root
          ~fanout
          ~interrupt
          ~check_info
          ~tcopt
          ~warnings_saved_state
      with
      | Success
          (diagnostics, map_reduce_data, dep_edges, warnings_saved_state, env)
        ->
        ( warnings_saved_state,
          { diagnostics; map_reduce_data; dep_edges; profiling_info },
          telemetry,
          env,
          None,
          None )
      | Cancel (env, reason) ->
        (* Typecheck is cancelled due to interrupt *)
        ( warnings_saved_state,
          {
            diagnostics = Diagnostics.empty;
            map_reduce_data = Map_reduce.empty;
            dep_edges = Typing_deps.dep_edges_make ();
            profiling_info;
          },
          telemetry,
          env,
          Some (original_fnl, reason),
          None )
      | DistCError msg -> exit_distc_failed ~phase:Runtime msg
    else (
      if check_info.log_errors then
        Server_progress.ErrorsWrite.telemetry
          (Telemetry.create ()
          |> Telemetry.bool_ ~key:"process_in_parallel" ~value:true);
      process_in_parallel
        ctx
        workers
        telemetry
        fnl
        ~interrupt
        ~memory_cap:(Some 500 (* megabytes *))
        ~longlived_workers
        ~check_info
        ~type_check_testing_hooks
        ~warnings_saved_state
        ~typecheck_info
    )
  in
  let { diagnostics; map_reduce_data; dep_edges; profiling_info } =
    typing_result
  in
  Typing_deps.register_discovered_dep_edges dep_edges;
  Map_reduce.finalize
    ~progress:(fun s -> Server_progress.write "%s" s)
    ~init_id:check_info.init_id
    ~recheck_id:check_info.recheck_id
    map_reduce_data;
  let telemetry =
    telemetry |> Telemetry.object_ ~key:"profiling_info" ~value:profiling_info
  in
  ( (env, { diagnostics; warnings_saved_state; telemetry; time_first_error }),
    cancelled_fnl_and_reason )

let go
    (ctx : Provider_context.t)
    (workers : Multi_worker.worker list option)
    (telemetry : Telemetry.t)
    (fnl : Relative_path.t list)
    ~(root : Path.t option)
    ~(longlived_workers : bool)
    ~(hh_distc_config : distc_config)
    ~(check_info : check_info)
    ~warnings_saved_state : result =
  let interrupt = Multi_threaded_call.no_interrupt () in
  let (((), result), unfinished_and_reason) =
    go_with_interrupt
      ctx
      workers
      telemetry
      fnl
      ~root
      ~interrupt
      ~longlived_workers
      ~hh_distc_config
      ~check_info
      ~type_check_testing_hooks:None
      ~warnings_saved_state
  in
  assert (Option.is_none unfinished_and_reason);
  result
