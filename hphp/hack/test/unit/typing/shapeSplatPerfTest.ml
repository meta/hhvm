(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Scaling benchmarks for the shape-splat merge (Typing_shape_normalize.merge)
 * and disjointness diagnostics (Typing_shape_disjointness.violations).
 *
 * Each scenario builds a splat of [n] elements and times the operation. Each
 * row reports the local exponent
 *   e = log (t_n / t_prev) / log (n / prev_n)
 * so e ~ 1 is linear and e ~ 2 is quadratic. Assertions use a log-log fit over
 * the largest sizes, with each time taken as the median of repeated samples.
 * The scenarios are chosen to isolate the two independent costs in a merge:
 *
 *   - the shape accumulator, exercised by the closed disjoint-field scenarios;
 *   - the disjointness accumulator (add_disjointness_operand), exercised by the
 *     generic / open / repeated-field scenarios, where nothing is merged into
 *     the shape accumulator at all.
 *
 * Run it directly to get the table:
 *   buck run @fbcode//mode/dev-nosan-lg \
 *     fbcode//hphp/hack/test/unit/typing:shapeSplatPerfTest -- \
 *     -runner sequential
 *)

open Hh_prelude
open OUnit2
open Typing_defs
module Env = Typing_env
module MakeType = Typing_make_type
module Reason = Typing_reason
module Norm = Typing_shape_normalize
module Disjointness = Typing_shape_disjointness

let dummy_env =
  let () = Typing_subtype.set_fun_refs () in
  let ctx =
    Provider_context.empty_for_test
      ~popt:Parser_options.default
      ~tcopt:Typechecker_options.default
      ~deps_mode:(Typing_deps_mode.InMemoryMode None)
  in
  let env = Typing_env_types.empty ctx Relative_path.default ~droot:None in
  let dummy_file = Relative_path.from_root ~suffix:"test.php" in
  let dummy_pos = Pos.make dummy_file (Lexing.from_string "") in
  let (env, _restore) = Env.set_inference_env_pos env (Some dummy_pos) in
  env

let r = Reason.none

let tint = MakeType.int r

let tnothing = MakeType.nothing r

let tmixed = MakeType.mixed r

let field ty = { sft_optional = false; sft_ty = ty }

let opt_field ty = { sft_optional = true; sft_ty = ty }

let key name = TShapeField.make (TSFlit_str (Pos_or_decl.none, name))

let simple ?(unknown = tnothing) kvs =
  {
    s_origin = Missing_origin;
    s_unknown_value = unknown;
    s_fields =
      List.fold kvs ~init:TShapeMap.empty ~f:(fun acc (k, v) ->
          TShapeMap.add (key k) v acc);
  }

let shape_ty s = mk (r, Tshape (Shape_simple s))

let splat elems = mk (r, Tshape (Shape_splat { ss_elems = elems }))

let closed_field i = shape_ty (simple [(Printf.sprintf "f%d" i, field tint)])

let open_field i =
  shape_ty (simple ~unknown:tmixed [(Printf.sprintf "f%d" i, field tint)])

let generic i = mk (r, Tgeneric (Printf.sprintf "T%d" i))

(* -- Inputs ---------------------------------------------------------------- *)

(* [shape(...shape('f0' => int), ..., ...shape('f{n-1}' => int))]: every element
   is a closed single-field shape and all field names are distinct, so the merge
   is a pure accumulate with no overlap to report. *)
let flat_disjoint n = List.init n ~f:closed_field

(* As above but every element carries the same field, so rightmost-wins
   overwrites at each step and every element overlaps every earlier one. *)
let flat_same_field n =
  List.init n ~f:(fun _ -> shape_ty (simple [("a", field tint)]))

(* [n] distinct type parameters. Nothing merges into the shape accumulator: the
   elements are all residual, so this isolates the disjointness bookkeeping. *)
let flat_generics n = List.init n ~f:generic

(* [n] open shapes with disjoint fields. Each contributes both a field and an
   unknown-fields upper bound. *)
let flat_open n =
  List.init n ~f:(fun i ->
      shape_ty (simple ~unknown:tmixed [(Printf.sprintf "f%d" i, field tint)]))

(* As [flat_open], but every field is optional. Each open row semantically
   widens all earlier optional fields, exercising deferred widening. *)
let flat_open_optional n =
  List.init n ~f:(fun i ->
      shape_ty
        (simple ~unknown:tmixed [(Printf.sprintf "f%d" i, opt_field tint)]))

(* [n] closed elements followed by a single open one. Isolates the cost of one
   open row meeting a large accumulator: linear if the walk is the only extra
   cost, quadratic only if every element pays it. *)
let closed_then_one_open n =
  List.init n ~f:closed_field
  @ [shape_ty (simple ~unknown:tmixed [("last", field tint)])]

(* A left-nested chain [shape(...shape(...shape(), 'f0' => int), 'f1' => int)...]
   of depth [n], the shape an alias chain expands to. *)
let left_nested n =
  let rec build i acc =
    if i >= n then
      [acc]
    else
      build (i + 1) (splat [acc; closed_field i])
  in
  build 0 (shape_ty (simple []))

(* The mirror image: [shape('f0' => int, ...shape('f1' => int, ...))]. *)
let right_nested n =
  let rec build i acc =
    if i >= n then
      [acc]
    else
      build (i + 1) (splat [closed_field i; acc])
  in
  build 0 (shape_ty (simple []))

(* Two shapes of [n] fields each, merged once: the linear-in-n baseline that the
   scenarios above should not be able to beat asymptotically. *)
let wide_pair n =
  let fields prefix =
    List.init n ~f:(fun i -> (Printf.sprintf "%s%d" prefix i, field tint))
  in
  [shape_ty (simple (fields "l")); shape_ty (simple (fields "r"))]

(* A nested splat whose flattened result has [n + 1] concrete shapes. This
   isolates repeated copying while flattening nested successful resolutions. *)
let concrete_supertypes_nested n =
  match left_nested n with
  | [ty] -> (dummy_env, ty)
  | _ -> failwith "left_nested must produce one root type"

(* [T0, ..., T{n-1}], where each [Ti] is bounded by [T{i+1}] and [Tn] is
   bounded by a concrete shape. Resolving every listed generic independently
   exposes repeated traversal of overlapping bound-chain suffixes. *)
let concrete_supertypes_generic_chain n =
  let name i = Printf.sprintf "T%d" i in
  let env = Env.add_upper_bound dummy_env (name n) (closed_field n) in
  let rec add_bounds env i =
    if i < 0 then
      env
    else
      add_bounds (Env.add_upper_bound env (name i) (generic (i + 1))) (i - 1)
  in
  let env = add_bounds env (n - 1) in
  (env, splat (List.init n ~f:generic))

(* A single generic with [n] compatible concrete upper bounds. Resolving the
   generic collects all bounds and intersects them before recursing on the
   result, exposing any repeated copying while building the intersection. *)
let concrete_supertypes_wide_generic_bounds n =
  let env =
    List.fold (List.init n ~f:open_field) ~init:dummy_env ~f:(fun env bound ->
        Env.add_upper_bound env "T0" bound)
  in
  (env, splat [generic 0])

(* The first element is enough to make resolution fail. The generic-chain
   suffix checks whether [resolve_elems] nevertheless resolves every remaining
   element after the result is already known to be [None]. *)
let concrete_supertypes_failure_before_generic_chain n =
  let (env, ty) = concrete_supertypes_generic_chain n in
  match get_node ty with
  | Tshape (Shape_splat { ss_elems }) -> (env, splat (tint :: ss_elems))
  | _ -> failwith "generic chain must produce a shape splat"

let shared_generic_bound_dag_input ~leaf depth =
  let name i = Printf.sprintf "T%d" i in
  let env = Env.add_upper_bound dummy_env (name depth) leaf in
  let rec add_bounds env i =
    if i < 0 then
      env
    else
      let next = generic (i + 1) in
      let env = Env.add_upper_bound env (name i) (splat [next; next]) in
      add_bounds env (i - 1)
  in
  let env = add_bounds env (depth - 1) in
  (env, splat [generic 0])

(* -- Harness --------------------------------------------------------------- *)

let sizes = [64; 128; 256; 512; 1024]

let run_merge elems =
  let (_env, _err, result) = Norm.merge ~on_error:None elems dummy_env in
  ignore (Sys.opaque_identity result)

let run_disjointness elems =
  let violations = Disjointness.violations elems dummy_env in
  ignore (Sys.opaque_identity violations)

let run_concrete_supertypes_success (env, ty) =
  let (_env, result) =
    Typing_utils.get_shape_splat_concrete_supertypes env ty
  in
  match result with
  | None -> assert_failure "expected concrete shape supertypes"
  | Some tys -> ignore (Sys.opaque_identity tys)

let run_concrete_supertypes_failure (env, ty) =
  let (_env, result) =
    Typing_utils.get_shape_splat_concrete_supertypes env ty
  in
  match result with
  | None -> ()
  | Some _ -> assert_failure "expected concrete shape resolution to fail"

let min_sample_seconds = 0.02

let timing_samples = 3

let fitted_size_count = 4

let median samples =
  let samples = List.sort samples ~compare:Float.compare in
  List.nth_exn samples (List.length samples / 2)

(* CPU seconds per run. Calibrate fast operations to a sufficiently long batch,
   then reuse that iteration count for independent samples. The discarded
   calibration batches together cost less than the final calibration batch. *)
let time ~samples f =
  let measure iterations =
    let t0 = Sys.time () in
    for _ = 1 to iterations do
      f ()
    done;
    let elapsed = Sys.time () -. t0 in
    (elapsed, elapsed /. float_of_int iterations)
  in
  Gc.compact ();
  let rec calibrate iterations =
    let (elapsed, per_run) = measure iterations in
    if Float.(elapsed >= min_sample_seconds) then
      (iterations, per_run)
    else
      calibrate (iterations * 2)
  in
  let (iterations, first_sample) = calibrate 1 in
  let remaining_samples =
    List.init (samples - 1) ~f:(fun _ ->
        Gc.compact ();
        measure iterations |> snd)
  in
  median (first_sample :: remaining_samples)

type measurement = {
  size: int;
  seconds: float;
}

let fit_exponent measurements =
  let reversed = List.rev measurements in
  let measurements = List.rev (List.take reversed fitted_size_count) in
  let points =
    List.map measurements ~f:(fun { size; seconds } ->
        (Float.log (float_of_int size), Float.log seconds))
  in
  let count = float_of_int (List.length points) in
  let (sum_x, sum_y) =
    List.fold points ~init:(0., 0.) ~f:(fun (sum_x, sum_y) (x, y) ->
        (sum_x +. x, sum_y +. y))
  in
  let mean_x = sum_x /. count in
  let mean_y = sum_y /. count in
  let (covariance, variance) =
    List.fold points ~init:(0., 0.) ~f:(fun (covariance, variance) (x, y) ->
        let dx = x -. mean_x in
        (covariance +. (dx *. (y -. mean_y)), variance +. (dx *. dx)))
  in
  covariance /. variance

(* Time [build n |> run] across [sizes] and return the exponent fitted over the
   largest [fitted_size_count] measurements. *)
let bench_with ~sizes ~samples run name build =
  Printf.printf "\n%s\n" name;
  Printf.printf
    "%8s %12s %14s %14s %10s\n"
    "n"
    "t (ms)"
    "t/n (us)"
    "t/n^2 (ns)"
    "local exp.";
  let prev = ref None in
  let measurements =
    List.map sizes ~f:(fun n ->
        let elems = build n in
        let t = time ~samples (fun () -> run elems) in
        let n_f = float_of_int n in
        let local_exponent =
          match !prev with
          | Some (prev_n, prev_t) ->
            Float.log (t /. prev_t) /. Float.log (n_f /. float_of_int prev_n)
          | None -> Float.nan
        in
        Printf.printf
          "%8d %12.2f %14.3f %14.3f %10s\n%!"
          n
          (t *. 1000.)
          (t /. n_f *. 1e6)
          (t /. (n_f *. n_f) *. 1e9)
          (if Float.is_nan local_exponent then
            "-"
          else
            Printf.sprintf "%.2f" local_exponent);
        prev := Some (n, t);
        { size = n; seconds = t })
  in
  let exponent = fit_exponent measurements in
  Printf.printf
    "fitted exponent over largest %d sizes: %.2f\n%!"
    fitted_size_count
    exponent;
  exponent

let bench ?(sizes = sizes) name build =
  bench_with ~sizes ~samples:timing_samples run_merge name build

let bench_disjointness name build =
  bench_with
    ~sizes
    ~samples:timing_samples
    run_disjointness
    ("disjointness_" ^ name)
    build

let bench_concrete_supertypes ?(sizes = sizes) name build =
  bench_with
    ~sizes
    ~samples:timing_samples
    run_concrete_supertypes_success
    ("concrete_supertypes_" ^ name)
    build

let bench_concrete_supertypes_failure name build =
  bench_with
    ~sizes
    ~samples:timing_samples
    run_concrete_supertypes_failure
    ("concrete_supertypes_" ^ name)
    build

(* Perf assertions are inherently noisy, so the threshold is deliberately slack:
   it is here to catch a return to quadratic scaling, not to police constant
   factors. A scenario that is genuinely linear fits near 1.0 and stays well
   below 1.5 once the fixed costs wash out. *)
let assert_subquadratic name exponent =
  assert_bool
    (Printf.sprintf
       "%s should scale sub-quadratically, fitted exponent %.2f"
       name
       exponent)
    (Float.is_finite exponent && Float.(exponent < 1.5))

let scaling_sizes = [1024; 2048; 4096; 8192; 16384]

let assert_scaling name build =
  assert_subquadratic name (bench ~sizes:scaling_sizes name build)

let perf_same_field _ = assert_scaling "flat_same_field" flat_same_field

let perf_generics _ = assert_scaling "flat_generics" flat_generics

let perf_wide_pair _ = assert_scaling "wide_pair" wide_pair

let perf_disjoint_fields _ = assert_scaling "flat_disjoint" flat_disjoint

let perf_disjointness_disjoint_fields _ =
  assert_subquadratic
    "disjointness_flat_disjoint"
    (bench_disjointness "flat_disjoint" flat_disjoint)

let perf_disjointness_same_field _ =
  assert_subquadratic
    "disjointness_flat_same_field"
    (bench_disjointness "flat_same_field" flat_same_field)

let perf_disjointness_generics _ =
  assert_subquadratic
    "disjointness_flat_generics"
    (bench_disjointness "flat_generics" flat_generics)

let perf_open _ = assert_scaling "flat_open" flat_open

let perf_open_optional _ =
  assert_scaling "flat_open_optional" flat_open_optional

let perf_closed_then_one_open _ =
  assert_subquadratic
    "closed_then_one_open"
    (bench "closed_then_one_open" closed_then_one_open)

let perf_left_nested _ =
  assert_subquadratic "left_nested" (bench "left_nested" left_nested)

let perf_right_nested _ =
  assert_subquadratic "right_nested" (bench "right_nested" right_nested)

let perf_concrete_supertypes_nested _ =
  assert_subquadratic
    "concrete_supertypes_nested"
    (bench_concrete_supertypes "nested" concrete_supertypes_nested)

let perf_concrete_supertypes_generic_chain _ =
  assert_subquadratic
    "concrete_supertypes_generic_chain"
    (bench_concrete_supertypes
       ~sizes:scaling_sizes
       "generic_chain"
       concrete_supertypes_generic_chain)

let perf_concrete_supertypes_wide_generic_bounds _ =
  assert_subquadratic
    "concrete_supertypes_wide_generic_bounds"
    (bench_concrete_supertypes
       "wide_generic_bounds"
       concrete_supertypes_wide_generic_bounds)

let perf_concrete_supertypes_failure_before_generic_chain _ =
  assert_subquadratic
    "concrete_supertypes_failure_before_generic_chain"
    (bench_concrete_supertypes_failure
       "failure_before_generic_chain"
       concrete_supertypes_failure_before_generic_chain)

let shared_generic_bound_dag_non_shape _ =
  let depth = 24 in
  let (env, ty) = shared_generic_bound_dag_input ~leaf:tint depth in
  let (_env, result) =
    Typing_utils.get_shape_splat_concrete_supertypes env ty
  in
  assert_equal None result

let shared_generic_bound_dag_shape _ =
  let depth = 16 in
  let (env, ty) =
    shared_generic_bound_dag_input ~leaf:(closed_field depth) depth
  in
  let (_env, result) =
    Typing_utils.get_shape_splat_concrete_supertypes env ty
  in
  assert_equal
    ~printer:Int.to_string
    (1 lsl depth)
    (Option.value_map result ~default:0 ~f:List.length)

let concrete_supertypes_cache_does_not_cache_cycles _ =
  let env = Env.add_upper_bound dummy_env "T0" (generic 1) in
  let env = Env.add_upper_bound env "T0" (closed_field 0) in
  let env = Env.add_upper_bound env "T1" (generic 0) in
  let env = Env.add_upper_bound env "T1" (closed_field 1) in
  let cache = Typing_utils.Concrete_supertypes_cache.create () in
  let (env, t0_supertypes) =
    Typing_utils.get_concrete_supertypes
      ~cache
      ~abstract_enum:false
      env
      (generic 0)
  in
  let (_env, t1_supertypes) =
    Typing_utils.get_concrete_supertypes
      ~cache
      ~abstract_enum:false
      env
      (generic 1)
  in
  assert_equal ~printer:Int.to_string 2 (List.length t0_supertypes);
  assert_equal ~printer:Int.to_string 2 (List.length t1_supertypes)

let concrete_supertypes_cache_is_environment_local _ =
  let cache = Typing_utils.Concrete_supertypes_cache.create () in
  let env = Env.add_upper_bound dummy_env "T0" (closed_field 0) in
  let (_env, initial_supertypes) =
    Typing_utils.get_concrete_supertypes
      ~cache
      ~abstract_enum:false
      env
      (generic 0)
  in
  let env = Env.add_upper_bound env "T0" (closed_field 1) in
  let (_env, extended_supertypes) =
    Typing_utils.get_concrete_supertypes
      ~cache
      ~abstract_enum:false
      env
      (generic 0)
  in
  assert_equal ~printer:Int.to_string 1 (List.length initial_supertypes);
  assert_equal ~printer:Int.to_string 2 (List.length extended_supertypes)

let () =
  "shapeSplatPerfTest"
  >::: [
         "concrete_supertypes_nested" >:: perf_concrete_supertypes_nested;
         "concrete_supertypes_generic_chain"
         >:: perf_concrete_supertypes_generic_chain;
         "concrete_supertypes_wide_generic_bounds"
         >:: perf_concrete_supertypes_wide_generic_bounds;
         "concrete_supertypes_failure_before_generic_chain"
         >:: perf_concrete_supertypes_failure_before_generic_chain;
         "shared_generic_bound_dag_non_shape"
         >:: shared_generic_bound_dag_non_shape;
         "shared_generic_bound_dag_shape" >:: shared_generic_bound_dag_shape;
         "concrete_supertypes_cache_does_not_cache_cycles"
         >:: concrete_supertypes_cache_does_not_cache_cycles;
         "concrete_supertypes_cache_is_environment_local"
         >:: concrete_supertypes_cache_is_environment_local;
         "wide_pair" >:: perf_wide_pair;
         "flat_same_field" >:: perf_same_field;
         "flat_generics" >:: perf_generics;
         "flat_disjoint" >:: perf_disjoint_fields;
         "disjointness_flat_disjoint" >:: perf_disjointness_disjoint_fields;
         "disjointness_flat_same_field" >:: perf_disjointness_same_field;
         "disjointness_flat_generics" >:: perf_disjointness_generics;
         "flat_open" >:: perf_open;
         "flat_open_optional" >:: perf_open_optional;
         "closed_then_one_open" >:: perf_closed_then_one_open;
         "left_nested" >:: perf_left_nested;
         "right_nested" >:: perf_right_nested;
       ]
  |> run_test_tt_main
