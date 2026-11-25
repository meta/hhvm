(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Checks on Typing_corners that do not involve deciding subtyping.

   When a shape spreads a type parameter, as in shape(...T), the checker does not
   know what fields T has. For each field name it instead works out the most and
   the least that T's bounds allow that field to be, and checks the subtyping
   against both. When one parameter's bound mentions another, the second has to
   be given a value first, or the most and the least for the first cannot be
   worked out at all. Typing_corners records which parameter needs which, and the
   tests here check that record is consistent.
*)

open Hh_prelude
open OUnit2
open Typing_defs
module Env = Typing_env
module MakeType = Typing_make_type
module Reason = Typing_reason

module Typing_corners = struct
  include Typing_corners
  include Typing_corners.For_test
end

let dummy_env () =
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

let expect_row normalized =
  match Typing_shape_normalize.Row.as_row normalized with
  | Some row -> row
  | None ->
    assert_failure "expected one normalized row, got a distributed result"

let normalize_row env shape =
  let (env, err, normalized) =
    Typing_shape_normalize.Row.normalize ~on_error:None r shape env
  in
  assert_equal None err;
  (env, expect_row normalized)

let tgeneric name = mk (r, Tgeneric name)

let param_name ty =
  match get_node ty with
  | Tgeneric n -> n
  | Tnewtype (n, _, _) -> n
  | _ -> "?"

let expect_ok = function
  | Ok value -> value
  | Error _ -> assert_failure "unexpected missing corner assignment"

let expect_computed = function
  | Typing_corners.Computed value -> value
  | Typing_corners.Empty -> assert_failure "unexpected empty corner computation"
  | Typing_corners.Unsupported_cycle _ ->
    assert_failure "unexpected unsupported corner cycle"

let names tys = List.map tys ~f:param_name |> List.sort ~compare:String.compare

let show names = String.concat ~sep:"," names

let splat elems = mk (r, Tshape (Shape_splat { ss_elems = elems }))

let field ty = { sft_optional = false; sft_ty = ty }

let optional_field ty = { sft_optional = true; sft_ty = ty }

let simple_shape fields ~open_ =
  mk
    ( r,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_unknown_value =
               (if open_ then
                 MakeType.mixed r
               else
                 MakeType.nothing r);
             s_fields =
               List.fold fields ~init:TShapeMap.empty ~f:(fun acc (l, ty) ->
                   TShapeMap.add
                     (TShapeField.make (TSFlit_str (Pos_or_decl.none, l)))
                     (field ty)
                     acc);
           }) )

let simple_shape_field name fd =
  mk
    ( r,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_unknown_value = MakeType.nothing r;
             s_fields =
               TShapeMap.singleton
                 (TShapeField.make (TSFlit_str (Pos_or_decl.none, name)))
                 fd;
           }) )

let open_shape = simple_shape [] ~open_:true

(* == The labels a check can be about ======================================= *)

(* Either a named field, or the "everything else" case covering every field name
   nobody wrote down. The second is a distinct path through the code and is where
   the first soundness bug in this area was found, so every check below runs over
   both. *)
let labels =
  [Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))); None]

let describe_label = function
  | None -> "the unnamed fields"
  | Some l -> Printf.sprintf "field %s" (TShapeField.name l)

(* == Where the parameter sits inside the bound ============================= *)

(* A bound can mention a parameter in two quite different ways. It can be spread
   into the shape, as in shape(...T), where T stands for a whole set of fields
   and has to be given a value before the shape can be worked out. Or it can be
   the type of one field, as in shape('x' => T), where it is an ordinary type
   like any other and needs no value.

   Only the first kind creates a dependency. Both kinds are listed here so that
   the tests check the second kind does NOT create one: recording a dependency
   that is not needed adds a spurious ordering constraint, and can make two
   parameters look mutually dependent when they are not. *)
type placement = {
  descr: string;
  build: locl_ty -> locl_ty;
  spread_into_shape: bool;
}

let placements =
  [
    { descr = "bare T"; build = (fun t -> t); spread_into_shape = true };
    {
      descr = "shape(...T)";
      build = (fun t -> splat [t]);
      spread_into_shape = true;
    };
    {
      descr = "shape(...T, 'x' => int)";
      build =
        (fun t -> splat [t; simple_shape [("x", MakeType.int r)] ~open_:false]);
      spread_into_shape = true;
    };
    {
      descr = "shape('x' => int, ...T)";
      build =
        (fun t -> splat [simple_shape [("x", MakeType.int r)] ~open_:false; t]);
      spread_into_shape = true;
    };
    {
      descr = "shape('x' => T)";
      build = (fun t -> simple_shape [("x", t)] ~open_:false);
      spread_into_shape = false;
    };
    {
      descr = "shape('x' => shape(...T))";
      build = (fun t -> simple_shape [("x", splat [t])] ~open_:false);
      spread_into_shape = false;
    };
    {
      descr = "supportdyn<shape(...T)>";
      build = (fun t -> MakeType.supportdyn r (splat [t]));
      spread_into_shape = true;
    };
    (* The shape is only reachable past another type constructor. Whether the
       parameter is still found depends on how far the code follows the bound. *)
    (* Reaching a shape past an intersection. Both members bound the element
       from above, so both are read and a parameter in either must be found. *)
    {
      descr = "(shape(...T) & shape('y' => int))";
      build =
        (fun t ->
          mk
            ( r,
              Tintersection
                [splat [t]; simple_shape [("y", MakeType.int r)] ~open_:true] ));
      spread_into_shape = true;
    };
    (* Reaching a shape past a union. This one is not symmetric. Below the
       element, a union means it is above BOTH members, so both are read. Above
       it, being below `A | B` does not put it below either, so there is no row
       to read and nothing to find. The checks below therefore ask only that
       whatever IS read has its parameters found, rather than assuming a
       direction. *)
    {
      descr = "(shape(...T) | shape('y' => int))";
      build =
        (fun t ->
          mk
            ( r,
              Tunion
                [splat [t]; simple_shape [("y", MakeType.int r)] ~open_:true] ));
      spread_into_shape = true;
    };
  ]

(* == The two ways of writing the same constraint =========================== *)

(* `T2 as T1` and `T1 super T2` say the same thing. They are written on opposite
   sides, so each is set up separately here. *)
type written_as =
  | Upper_bound_on_t2
  | Lower_bound_on_t1

(* Only one side of the relation is recorded, because that is all the functions
   used here do. When the typechecker reads either spelling it records both
   sides, and it does so through a routine that detects circular reasoning and
   stops. Recording both sides directly skips that routine, produces a set of
   bounds the typechecker would never build, and makes working out a parameter's
   combined upper bound run forever. Any test that builds bounds by hand has to
   stay within what the typechecker could itself produce. *)
let set_up_bounds env written_as ~place =
  match written_as with
  | Upper_bound_on_t2 ->
    let env = Env.add_upper_bound env "T1" open_shape in
    Env.add_upper_bound env "T2" (place (tgeneric "T1"))
  | Lower_bound_on_t1 ->
    let env = Env.add_upper_bound env "T1" open_shape in
    let env = Env.add_upper_bound env "T2" open_shape in
    Env.add_lower_bound env "T1" (place (tgeneric "T2"))

let describe_written_as = function
  | Upper_bound_on_t2 -> "T2 as <bound>"
  | Lower_bound_on_t1 -> "T1 super <bound>"

(* == Every parameter a bound contributes must be recorded ==================
 *
 * To find what a parameter's bound says about a field, the code turns that bound
 * into a shape and reads the field off it. If the bound turns into a shape that
 * itself spreads another parameter, that other parameter is looked up by name,
 * and the lookup only succeeds if it was given a value first. Giving values in
 * the right order relies on the recorded dependencies, so anything a bound can
 * contribute must be recorded.
 *
 * A missing assignment is an analysis failure: treating it as absent would
 * claim more than is known. Complete dependency records prevent that failure.
 *
 * The check gives every parameter a value beforehand, because the bound is only
 * turned into a shape for parameters that already have one. *)

let assign_all_parameters keys =
  List.fold keys ~init:Typing_corners.Splat_elem.Map.empty ~f:(fun acc k ->
      Typing_corners.Splat_elem.Map.add k (field (MakeType.mixed r)) acc)

(* Worked out here rather than by calling the module's own [spread_params], so
   that what a view contains is judged independently of the code that decides
   which parameters get recorded. *)
let parameters_in_row row =
  Typing_shape_normalize.Row.fold
    row
    ~bottom:(fun () -> [])
    ~simple:(fun _ -> [])
    ~elements:(fun elements ->
      List.filter_map elements ~f:(fun element ->
          let ty = Typing_shape_normalize.Row.Element.ty element in
          match get_node ty with
          | Tgeneric _
          | Tnewtype _ ->
            Some ty
          | _ -> None))

let parameters_in_upper_bound_shape view =
  match view with
  | Typing_corners.Upper_shapes shapes ->
    List.concat_map shapes ~f:parameters_in_row
  | Typing_corners.Upper_bottom
  | Typing_corners.Upper_unconstrained ->
    []

let parameters_in_lower_bound_shape view =
  match view with
  | Typing_corners.Lower_shapes shapes ->
    List.concat_map shapes ~f:parameters_in_row
  | Typing_corners.Lower_bottom -> []

let check_bound_parameters_are_recorded env ~situation param =
  let assignment = assign_all_parameters [tgeneric "T1"; tgeneric "T2"] in
  let (_env, from_upper) =
    Typing_corners.bound_shape_upper env param assignment r
  in
  let (_env, from_lower) =
    Typing_corners.bound_shape_lower env param assignment r
  in
  let check ~which ~contributed ~recorded =
    let contributed = names contributed in
    let recorded = names recorded in
    List.iter contributed ~f:(fun p ->
        (* Either recorded as a dependency, so it will be given a value first,
           or already given one. Both make the lookup succeed; what must not
           happen is a parameter that is neither. *)
        let already_given =
          List.mem
            (names
               (List.map
                  (Typing_corners.Splat_elem.Map.bindings assignment)
                  ~f:fst))
            p
            ~equal:String.equal
        in
        assert_bool
          (Printf.sprintf
             "In %s, %s's %s bound contributes %s to a shape, but %s is neither recorded as a dependency (recorded: %s) nor already given a value.\nNothing then guarantees it has one when the shape is read, and the code then treats the field as absent, which lets wrong programs through."
             situation
             (param_name param)
             which
             p
             p
             (show recorded))
          (List.mem recorded p ~equal:String.equal || already_given))
  in
  check
    ~which:"upper"
    ~contributed:(parameters_in_upper_bound_shape from_upper)
    ~recorded:(Typing_corners.type_params_in_upper_bound env param r);
  check
    ~which:"lower"
    ~contributed:(parameters_in_lower_bound_shape from_lower)
    ~recorded:(Typing_corners.type_params_in_lower_bound env param r)

let every_parameter_in_a_bound_is_recorded _ =
  List.iter placements ~f:(fun place ->
      List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
          let env =
            set_up_bounds (dummy_env ()) written_as ~place:place.build
          in
          let situation =
            Printf.sprintf
              "%s written as %s"
              place.descr
              (describe_written_as written_as)
          in
          check_bound_parameters_are_recorded env ~situation (tgeneric "T1");
          check_bound_parameters_are_recorded env ~situation (tgeneric "T2")))

(* == A parameter spread into a bound must be found =========================
 *
 * The mirror of the check below, and the one that matters most. All the other
 * checks here still hold if nothing at all is recorded, which is how a real bug
 * once went unnoticed: bounds that were a parameter contributed no dependency,
 * the ordering had nothing to order, and everything looked fine.
 *
 * The requirement applies to a lower bound only when that bound is actually
 * read as a row. A parameter written as a lower bound on its own yields nothing
 * to read, by design, so there is nothing to order. Where a row IS read, any
 * parameter in it must be found. *)
let a_spread_parameter_is_found _ =
  List.iter placements ~f:(fun place ->
      if place.spread_into_shape then
        List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
            let env =
              set_up_bounds (dummy_env ()) written_as ~place:place.build
            in
            (* The requirement only applies where the bound is actually read
               as a row. A bare parameter as a lower bound, or a union as an
               upper one, yields no row on that side, so nothing is ever looked
               up there and nothing needs ordering. *)
            let bound_is_read =
              match written_as with
              | Upper_bound_on_t2 ->
                (match
                   Typing_corners.bound_shape_upper
                     env
                     (tgeneric "T2")
                     Typing_corners.Splat_elem.Map.empty
                     r
                 with
                | (_, Typing_corners.Upper_shapes _) -> true
                | (_, Typing_corners.Upper_bottom)
                | (_, Typing_corners.Upper_unconstrained) ->
                  false)
              | Lower_bound_on_t1 ->
                (match
                   Typing_corners.bound_shape_lower
                     env
                     (tgeneric "T1")
                     Typing_corners.Splat_elem.Map.empty
                     r
                 with
                | (_, Typing_corners.Lower_shapes _) -> true
                | (_, Typing_corners.Lower_bottom) -> false)
            in
            if bound_is_read then
              let (holder, expected) =
                match written_as with
                | Upper_bound_on_t2 -> (tgeneric "T2", "T1")
                | Lower_bound_on_t1 -> (tgeneric "T1", "T2")
              in
              let recorded =
                names (Typing_corners.type_params_in_bounds env holder r)
              in
              assert_bool
                (Printf.sprintf
                   "In %s written as %s, %s is spread into %s's bound but was not found: recorded dependencies are %s. Nothing then orders the two, and every other check here would still pass."
                   place.descr
                   (describe_written_as written_as)
                   expected
                   (param_name holder)
                   (show recorded))
                (List.mem recorded expected ~equal:String.equal)))

let a_spread_parameter_in_a_nested_intersection_is_found _ =
  let dependency = tgeneric "T1" in
  let nested_intersection =
    mk
      ( r,
        Tintersection
          [splat [dependency]; simple_shape [("y", MakeType.int r)] ~open_:true]
      )
  in
  let bound =
    mk
      ( r,
        Tunion
          [
            nested_intersection;
            simple_shape [("z", MakeType.bool r)] ~open_:true;
          ] )
  in
  let env = Env.add_upper_bound (dummy_env ()) "T2" bound in
  let recorded =
    names (Typing_corners.type_params_in_upper_bound env (tgeneric "T2") r)
  in
  assert_equal ~printer:show ["T1"] recorded

(* == A parameter used as a field type must not be recorded ================= *)

(* The converse of the check above. A parameter that is only the type of a field
   is never looked up while working out the shape, so recording it would impose
   an ordering for no reason, and two parameters that each mention the other in a
   field could then look mutually dependent and defeat the ordering entirely. *)
let a_field_type_is_not_a_dependency _ =
  List.iter placements ~f:(fun place ->
      if not place.spread_into_shape then
        List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
            let env =
              set_up_bounds (dummy_env ()) written_as ~place:place.build
            in
            List.iter
              [tgeneric "T1"; tgeneric "T2"]
              ~f:(fun param ->
                let recorded =
                  names (Typing_corners.type_params_in_bounds env param r)
                in
                assert_bool
                  (Printf.sprintf
                     "In %s written as %s, %s records dependencies %s, but the other parameter appears only as the type of a field, which is never looked up while working out the shape."
                     place.descr
                     (describe_written_as written_as)
                     (param_name param)
                     (show recorded))
                  (List.is_empty recorded))))

(* == Following bounds repeatedly reaches everything ========================
 *
 * Typing_corners.closure starts from a set of parameters and repeatedly adds
 * whatever their bounds depend on. The result has to be complete: if it contains
 * a parameter, it must also contain everything that parameter depends on, or a
 * parameter could be left without a value when it is needed. *)

let following_bounds_reaches_everything _ =
  List.iter placements ~f:(fun place ->
      List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
          let env =
            set_up_bounds (dummy_env ()) written_as ~place:place.build
          in
          let start =
            Typing_corners.Splat_elem.Set.of_list [tgeneric "T1"; tgeneric "T2"]
          in
          let reached = Typing_corners.For_test.closure env start r in
          Typing_corners.Splat_elem.Set.iter
            (fun param ->
              List.iter
                (Typing_corners.type_params_in_bounds env param r)
                ~f:(fun needed ->
                  assert_bool
                    (Printf.sprintf
                       "In %s written as %s, %s depends on %s, but %s was not reached by following bounds from the starting set."
                       place.descr
                       (describe_written_as written_as)
                       (param_name param)
                       (param_name needed)
                       (param_name needed))
                    (Typing_corners.Splat_elem.Set.mem needed reached)))
            reached))

(* == Giving values in an order that works ==================================
 *
 * Typing_corners.topo puts the parameters in the order they should be given
 * values. Two things must hold. Every parameter reached by following bounds must
 * appear exactly once, or one would be left without a value. And when the
 * dependencies form no cycle, each parameter must come after everything it
 * depends on, since that is the whole point of the ordering.
 *
 * When they DO form a cycle, as `where T1 = T2` produces, no order can satisfy
 * everything and the code just drops one of the dependencies. There is no right
 * answer to check there, so only the first requirement is checked. *)

let has_cycle env within =
  let rec visit param seen =
    if
      List.mem seen param ~equal:(fun a b ->
          String.equal (param_name a) (param_name b))
    then
      true
    else
      List.exists
        (Typing_corners.type_params_in_bounds env param r)
        ~f:(fun needed ->
          Typing_corners.Splat_elem.Set.mem needed within
          && visit needed (param :: seen))
  in
  Typing_corners.Splat_elem.Set.exists (fun p -> visit p []) within

let the_order_gives_values_before_they_are_needed _ =
  List.iter placements ~f:(fun place ->
      List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
          let env =
            set_up_bounds (dummy_env ()) written_as ~place:place.build
          in
          let start =
            Typing_corners.Splat_elem.Set.of_list [tgeneric "T1"; tgeneric "T2"]
          in
          let reached = Typing_corners.For_test.closure env start r in
          let order =
            let cache = Typing_corners.Cache.create () in
            Typing_corners.topo cache env start r
          in
          let situation =
            Printf.sprintf
              "%s written as %s"
              place.descr
              (describe_written_as written_as)
          in
          (* Everything reached appears, once each. *)
          let ordered = names order in
          let expected =
            names (Typing_corners.Splat_elem.Set.elements reached)
          in
          assert_bool
            (Printf.sprintf
               "In %s, the order is %s but the parameters needing values are %s."
               situation
               (show ordered)
               (show expected))
            (Int.equal
               (List.length order)
               (Typing_corners.Splat_elem.Set.cardinal reached)
            && Typing_corners.Splat_elem.Set.equal
                 (Typing_corners.Splat_elem.Set.of_list order)
                 reached);
          (* With no cycle, nothing comes before something it depends on. *)
          if not (has_cycle env reached) then
            List.iteri order ~f:(fun i param ->
                List.iter
                  (Typing_corners.type_params_in_bounds env param r)
                  ~f:(fun needed ->
                    match
                      List.findi order ~f:(fun _ q ->
                          String.equal (param_name q) (param_name needed))
                    with
                    | None -> ()
                    | Some (j, _) ->
                      assert_bool
                        (Printf.sprintf
                           "In %s, %s is given a value before %s, which it depends on. Order: %s."
                           situation
                           (param_name param)
                           (param_name needed)
                           (show (names order)))
                        (j < i)))))

let independent_roots_keep_the_assignment_frontier_narrow _ =
  let count = 12 in
  let chains =
    List.map (List.range 0 count) ~f:(fun i ->
        ( tgeneric (Printf.sprintf "A%02d" i),
          tgeneric (Printf.sprintf "Z%02d" i) ))
  in
  let env =
    List.fold chains ~init:(dummy_env ()) ~f:(fun env (dependency, root) ->
        let env = Env.add_upper_bound env (param_name dependency) open_shape in
        Env.add_upper_bound env (param_name root) (splat [dependency]))
  in
  let roots = Typing_corners.Splat_elem.Set.of_list (List.map chains ~f:snd) in
  let reached = Typing_corners.For_test.closure env roots r in
  let run () =
    let cache = Typing_corners.Cache.create () in
    Typing_corners.topo cache env roots r
  in
  let order = run () in
  assert_bool
    "root-seeded ordering must include every reachable parameter exactly once"
    (Int.equal
       (List.length order)
       (Typing_corners.Splat_elem.Set.cardinal reached)
    && Typing_corners.Splat_elem.Set.equal
         (Typing_corners.Splat_elem.Set.of_list order)
         reached);
  let same left right =
    Int.equal 0 (Typing_corners.Splat_elem.compare left right)
  in
  let (pending, max_pending) =
    List.fold
      order
      ~init:(Typing_corners.Splat_elem.Set.empty, 0)
      ~f:(fun (pending, max_pending) key ->
        match
          List.find chains ~f:(fun (dependency, _) -> same key dependency)
        with
        | Some (dependency, _) ->
          let pending = Typing_corners.Splat_elem.Set.add dependency pending in
          ( pending,
            Int.max max_pending (Typing_corners.Splat_elem.Set.cardinal pending)
          )
        | None ->
          let (dependency, _) =
            Option.value_exn
              (List.find chains ~f:(fun (_, root) -> same key root))
          in
          assert_bool
            "a root must be ordered after its dependency"
            (Typing_corners.Splat_elem.Set.mem dependency pending);
          (Typing_corners.Splat_elem.Set.remove dependency pending, max_pending))
  in
  assert_equal 0 (Typing_corners.Splat_elem.Set.cardinal pending);
  assert_equal 1 max_pending;
  assert_bool
    "root-seeded ordering must be deterministic"
    (List.equal same order (run ()))

module Projected_pair = struct
  type t = locl_phase shape_field_type * locl_phase shape_field_type

  let compare_field left right =
    let optional = Bool.compare left.sft_optional right.sft_optional in
    if Int.equal optional 0 then
      compare_locl_ty ?normalize_lists:None left.sft_ty right.sft_ty
    else
      optional

  let compare (sub_left, super_left) (sub_right, super_right) =
    let sub = compare_field sub_left sub_right in
    if Int.equal sub 0 then
      compare_field super_left super_right
    else
      sub

  module Set = Stdlib.Set.Make (struct
    type nonrec t = t

    let compare = compare
  end)
end

let projected_pairs env sub super label assignments =
  List.fold
    assignments
    ~init:Projected_pair.Set.empty
    ~f:(fun pairs assignment ->
      let (env, sub_field) = Typing_corners.proj env sub label assignment in
      let sub_field = expect_ok sub_field in
      let (_env, super_field) =
        Typing_corners.proj env super label assignment
      in
      let super_field = expect_ok super_field in
      Projected_pair.Set.add (sub_field, super_field) pairs)

let params prefix count =
  List.map (List.range 0 count) ~f:(fun i ->
      tgeneric (Printf.sprintf "%s%02d" prefix i))

let assignment_frontier_for_rows env sub super params =
  let live = Typing_corners.Splat_elem.Set.of_list params in
  let cache = Typing_corners.Cache.create () in
  let label = Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))) in
  let (_env, assignments) =
    Typing_corners.corner_assignments cache env live label r
  in
  let assignments = assignments |> expect_ok |> expect_computed in
  (sub, super, label, assignments)

let assignment_frontier env sub_params super_params =
  let (env, sub) = normalize_row env (Shape_splat { ss_elems = sub_params }) in
  let (env, super) =
    normalize_row env (Shape_splat { ss_elems = super_params })
  in
  assignment_frontier_for_rows env sub super (sub_params @ super_params)

let add_optional_marker_bounds env param =
  let marker = tgeneric ("M" ^ param_name param) in
  let lower = simple_shape_field "x" (optional_field (MakeType.nothing r))
  and upper = simple_shape_field "x" (optional_field marker) in
  let env = Env.add_lower_bound env (param_name param) lower in
  Env.add_upper_bound env (param_name param) upper

let required_fields_collapse_the_assignment_frontier _ =
  let count = 5 in
  let sub_params = params "A" count and super_params = params "B" count in
  let upper = simple_shape_field "x" (field (MakeType.mixed r)) in
  let env =
    List.fold
      (sub_params @ super_params)
      ~init:(dummy_env ())
      ~f:(fun env param -> Env.add_upper_bound env (param_name param) upper)
  in
  let (sub, super, label, assignments) =
    assignment_frontier env sub_params super_params
  in
  let projected = projected_pairs env sub super label assignments in
  assert_equal 4 (List.length assignments);
  assert_equal 4 (Projected_pair.Set.cardinal projected)

let distinct_optional_subsets_remain_distinct _ =
  let count = 4 in
  let sub_params = params "A" count and super_params = params "B" count in
  let env =
    List.fold
      (sub_params @ super_params)
      ~init:(dummy_env ())
      ~f:add_optional_marker_bounds
  in
  let (sub, super, label, assignments) =
    assignment_frontier env sub_params super_params
  in
  let expected = Int.pow 4 count in
  let projected = projected_pairs env sub super label assignments in
  assert_equal expected (List.length assignments);
  assert_equal expected (Projected_pair.Set.cardinal projected)

let open_tails_collapse_optional_assignment_frontiers _ =
  let count = 12 in
  let sub_params = params "A" count and super_params = params "B" count in
  let all_params = sub_params @ super_params in
  let env =
    List.fold all_params ~init:(dummy_env ()) ~f:add_optional_marker_bounds
  in
  let row params = Shape_splat { ss_elems = List.rev params @ [open_shape] } in
  let (env, sub) = normalize_row env (row sub_params) in
  let (env, super) = normalize_row env (row super_params) in
  let (sub, super, label, assignments) =
    assignment_frontier_for_rows env sub super all_params
  in
  let projected = projected_pairs env sub super label assignments in
  assert_equal 1 (List.length assignments);
  assert_equal 1 (Projected_pair.Set.cardinal projected)

let other = tgeneric "T2"

(* Every row is used as the BOUND, but not as the neighbour sitting beside the
   parameter. What a neighbour changes is only whether it is required,
   optional or absent at the label, and whether the row is closed or open,
   since that is all liveness and masking look at. Those six cover every
   distinct case; using all rows here instead multiplies the work by fifteen
   and adds no configuration. *)
let neighbours =
  List.concat_map [false; true] ~f:(fun open_ ->
      [
        simple_shape [] ~open_;
        simple_shape [("x", MakeType.int r)] ~open_;
        simple_shape
          [("x", simple_shape [("p", MakeType.int r)] ~open_:false)]
          ~open_;
        simple_shape [("x", splat [tgeneric "T1"])] ~open_;
        mk
          ( r,
            Tshape
              (Shape_simple
                 {
                   s_origin = Missing_origin;
                   s_unknown_value =
                     (if open_ then
                       MakeType.mixed r
                     else
                       MakeType.nothing r);
                   s_fields =
                     TShapeMap.singleton
                       (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x")))
                       { sft_optional = true; sft_ty = MakeType.int r };
                 }) );
      ])

(* What the other parameter may already have been given. The empty case is the
   first one; the rest exercise a corner worked out UNDER another parameter's
   value, which is the configuration two separate bugs turned on. *)
let other_fields =
  [
    { sft_optional = true; sft_ty = MakeType.nothing r };
    { sft_optional = false; sft_ty = MakeType.nothing r };
    { sft_optional = false; sft_ty = MakeType.int r };
    { sft_optional = true; sft_ty = MakeType.int r };
    { sft_optional = true; sft_ty = MakeType.mixed r };
  ]

let assignments_for_other =
  Typing_corners.Splat_elem.Map.empty
  :: List.map other_fields ~f:(fun fd ->
         Typing_corners.Splat_elem.Map.singleton other fd)

(* Every row over one label, from a fixed set of field types. This is the whole
   universe the pruning check runs over: every field state a label can be in,
   crossed with the row being closed or open. *)
let field_types =
  [
    MakeType.nothing r;
    MakeType.int r;
    MakeType.bool r;
    (* A field whose type is itself a shape. Rows are read one label at a time,
       so a nested shape is only ever compared as a whole, but it is a different
       comparison from a scalar and the code has no special case for it.

       once at source level instead, in self_referential_bound.php. *)
    simple_shape [("p", MakeType.int r)] ~open_:false;
    (* A field whose type spreads the parameter under test. Fine as part of a
       row being compared; ruinous as the parameter's own BOUND, since working
       out a field range for the parameter then needs the field range of the
       same parameter again. Excluded from the bounds below for that reason, and
       only there: the type still appears throughout the rows. *)
    splat [tgeneric "T1"];
  ]

let field_states =
  None (* the label is absent *)
  :: List.concat_map field_types ~f:(fun ty ->
         [
           Some { sft_optional = false; sft_ty = ty };
           Some { sft_optional = true; sft_ty = ty };
         ])

let all_rows =
  List.concat_map field_states ~f:(fun state ->
      List.map [false; true] ~f:(fun open_ ->
          let fields =
            match state with
            | None -> []
            | Some fd ->
              [(TShapeField.make (TSFlit_str (Pos_or_decl.none, "x")), fd)]
          in
          {
            s_origin = Missing_origin;
            s_unknown_value =
              (if open_ then
                MakeType.mixed r
              else
                MakeType.nothing r);
            s_fields =
              List.fold fields ~init:TShapeMap.empty ~f:(fun acc (k, v) ->
                  TShapeMap.add k v acc);
          }))

let show_row (row : locl_phase shape_type_simple) =
  Printf.sprintf
    "shape(%s%s)"
    (String.concat
       ~sep:", "
       (List.map (TShapeMap.bindings row.s_fields) ~f:(fun (k, fd) ->
            Printf.sprintf
              "%s%s => _"
              (if fd.sft_optional then
                "?"
              else
                "")
              (TShapeField.name k))))
    (if Typing_defs.is_nothing row.s_unknown_value then
      ""
    else
      ", ...")

(* == Saying a field is hidden, and saying it is not ========================
 *
 * Fields are read right to left, so an element further right that definitely has
 * the field hides whatever the ones to its left say about it. Typing_corners
 * reports that as Masked and the checker then skips the hidden work, Unmasked
 * when nothing hides it, and Unknown when it cannot tell.
 *
 * Both definite answers are checked against the row itself. Claiming Masked when
 * nothing hides the field skips work that was needed. Claiming Unmasked when
 * something does hide it is the same mistake the other way round: work is done
 * against an element whose contribution has already been overridden. Unknown
 * claims nothing and so needs no check. *)

(* Only a PARAMETER to the right can mask. A concrete element to the right is
   already accounted for when the row is read, since reading takes the rightmost
   value, so masking is not about those. A parameter masks when its bound says
   the field is definitely there. *)
let a_parameter_to_the_right_definitely_has_the_field
    env row label key assignment =
  match row with
  | Shape_simple _ -> false
  | Shape_splat { ss_elems } ->
    let rec after = function
      | [] -> []
      | ty :: rest ->
        if Int.equal (Typing_corners.Splat_elem.compare ty key) 0 then
          rest
        else
          after rest
    in
    List.exists (after ss_elems) ~f:(fun ty ->
        match get_node ty with
        | Tgeneric _
        | Tnewtype _ ->
          let (_env, bounds) =
            Typing_corners.field_bounds env ty label assignment r
          in
          let (_lower, upper) = expect_ok bounds in
          not upper.sft_optional
        | _ -> false)

let a_field_is_hidden_exactly_when_something_hides_it _ =
  let key = tgeneric "T1" in
  let other = tgeneric "T2" in
  List.iter labels ~f:(fun label ->
      (* Every arrangement of the parameter with a neighbour on either side,
         over the same neighbours the corner check uses, plus a second
         parameter, whose own field may or may not be known. *)
      let rows =
        [("...T1", Shape_splat { ss_elems = [key] })]
        @ List.concat_mapi neighbours ~f:(fun i as_ty ->
              [
                ( Printf.sprintf "...T1, n%d" i,
                  Shape_splat { ss_elems = [key; as_ty] } );
                ( Printf.sprintf "n%d, ...T1" i,
                  Shape_splat { ss_elems = [as_ty; key] } );
              ])
        @ [
            ("...T1, ...T2", Shape_splat { ss_elems = [key; other] });
            ("...T2, ...T1", Shape_splat { ss_elems = [other; key] });
          ]
      in
      List.iter all_rows ~f:(fun bound_row ->
          let env =
            Env.add_upper_bound
              (dummy_env ())
              "T1"
              (mk (r, Tshape (Shape_simple bound_row)))
          in
          let env = Env.add_upper_bound env "T2" open_shape in
          List.iter assignments_for_other ~f:(fun given ->
              let assignment =
                Typing_corners.Splat_elem.Map.add
                  key
                  { sft_optional = true; sft_ty = MakeType.mixed r }
                  given
              in
              List.iter rows ~f:(fun (descr, row) ->
                  let (env, normalized_row) = normalize_row env row in
                  let hidden =
                    a_parameter_to_the_right_definitely_has_the_field
                      env
                      row
                      label
                      key
                      assignment
                  in
                  let cache = Typing_corners.Cache.create () in
                  match
                    Typing_corners.Masking.of_row
                      cache
                      env
                      normalized_row
                      label
                      key
                      assignment
                      r
                    |> expect_ok
                  with
                  | Typing_corners.Masking.Masked ->
                    assert_bool
                      (Printf.sprintf
                         "For %s at %s, T1 is reported as hidden, but no parameter to its right definitely has that field. Work that was needed would be skipped."
                         descr
                         (describe_label label))
                      hidden
                  | Typing_corners.Masking.Unmasked ->
                    assert_bool
                      (Printf.sprintf
                         "For %s at %s, T1 is reported as not hidden, but a parameter to its right definitely has that field and overrides it."
                         descr
                         (describe_label label))
                      (not hidden)
                  | Typing_corners.Masking.Unknown -> ()))))

(* == Checking fewer corners must not let more programs through =============
 *
 * A parameter's field is checked at the extreme values its bounds allow. When
 * the parameter can only affect one side of the comparison, Typing_corners
 * checks one extreme instead of all of them, which is faster. That shortcut is
 * only safe if it never accepts something the full set of extremes would have
 * rejected.
 *
 * Here the full set is checked by hand and compared against the shortcut. The
 * per-field comparison is the same rule the checker uses: the field on the left
 * must be at least as required as the one on the right, and its type must be a
 * subtype. *)

(* == Checking fewer corners must not let more programs through =============
 *
 * A parameter's field is checked at the extreme values its bounds allow. When
 * the parameter can only affect one side of the comparison, Typing_corners
 * checks one extreme instead of all of them. That shortcut is only safe if it
 * never accepts something the full set of extremes would have rejected.
 *
 * Run over every row in the universe above as the parameter's bound, every
 * arrangement of the parameter with every row on each side, both the named
 * field and the unnamed ones, and both with and without something depending on
 * the parameter. *)

let field_holds env ~sub ~super =
  Typing_corners.Field.requiredness_lte ~sub ~super
  && Typing_subtype.is_sub_type env sub.sft_ty super.sft_ty

let obligation_holds_at env ~sub_row ~super_row ~label ~key ~given corner =
  let assignments =
    if Typing_corners.Splat_elem.Map.mem other given then
      [given]
    else
      List.map other_fields ~f:(fun fd ->
          Typing_corners.Splat_elem.Map.singleton other fd)
  in
  List.for_all assignments ~f:(fun assignment ->
      let assignment =
        Typing_corners.Splat_elem.Map.add key corner assignment
      in
      let (env, sub_field) = Typing_corners.proj env sub_row label assignment in
      let sub_field = expect_ok sub_field in
      let (_env, super_field) =
        Typing_corners.proj env super_row label assignment
      in
      let super_field = expect_ok super_field in
      field_holds env ~sub:sub_field ~super:super_field)

let checking_fewer_corners_accepts_no_more _ =
  let key = tgeneric "T1" in
  (* Every way the parameter can sit relative to a concrete row, plus the row on
     its own so that one side may not mention the parameter at all. *)
  let arrangements =
    [
      Shape_splat { ss_elems = [key] };
      (* A second parameter beside the first, on either side of it. Masking and
         the shortcuts read the other elements of the row, so another parameter
         is a different case from a concrete neighbour. *)
      Shape_splat { ss_elems = [key; other] };
      Shape_splat { ss_elems = [other; key] };
    ]
    @ List.concat_map neighbours ~f:(fun as_ty ->
          [
            Shape_splat { ss_elems = [key; as_ty] };
            Shape_splat { ss_elems = [as_ty; key] };
            (match get_node as_ty with
            | Tshape st -> st
            | _ -> assert false);
          ])
  in
  let describe = function
    | Shape_simple row -> show_row row
    | Shape_splat { ss_elems } ->
      String.concat
        ~sep:", "
        (List.map ss_elems ~f:(fun ty ->
             match get_node ty with
             | Tgeneric n -> "..." ^ n
             | Tshape (Shape_simple row) -> "..." ^ show_row row
             | _ -> "..._"))
  in
  (* Every row is a candidate bound EXCEPT one that mentions the parameter it
     bounds; see the note on the field types. Hack accepts such a bound and
     handles it, and self_referential_bound.php covers it. *)
  let mentions_param (row : locl_phase shape_type_simple) =
    List.exists (TShapeMap.bindings row.s_fields) ~f:(fun (_, fd) ->
        Typing_defs.ty_equal fd.sft_ty (splat [tgeneric "T1"]))
  in
  let bound_rows =
    List.filter all_rows ~f:(fun row -> not (mentions_param row))
  in
  List.iter labels ~f:(fun label ->
      List.iter bound_rows ~f:(fun bound_row ->
          let bound = mk (r, Tshape (Shape_simple bound_row)) in
          let base = Env.add_upper_bound (dummy_env ()) "T1" bound in
          List.iter arrangements ~f:(fun sub_shape ->
              let (env, sub_row) = normalize_row base sub_shape in
              List.iter arrangements ~f:(fun super_shape ->
                  let (env, super_row) = normalize_row env super_shape in
                  List.iter assignments_for_other ~f:(fun given ->
                      List.iter [false; true] ~f:(fun depended ->
                          let depended_on =
                            if depended then
                              Typing_corners.Splat_elem.Set.singleton key
                            else
                              Typing_corners.Splat_elem.Set.empty
                          in
                          let live_sub =
                            Typing_corners.Splat_elem.Set.of_list
                              (Typing_corners.row_live_spread_at sub_row label)
                          in
                          let live_super =
                            Typing_corners.Splat_elem.Set.of_list
                              (Typing_corners.row_live_spread_at
                                 super_row
                                 label)
                          in
                          if
                            Typing_corners.Splat_elem.Set.mem key live_sub
                            || Typing_corners.Splat_elem.Set.mem key live_super
                          then begin
                            let (env, bounds) =
                              Typing_corners.field_bounds env key label given r
                            in
                            let (lower, upper) = expect_ok bounds in
                            let all_corners =
                              match
                                Typing_corners.Field.Corners.of_bounds
                                  ~lower
                                  ~upper
                              with
                              | Typing_corners.Field.Corners.Values cs -> cs
                              | Typing_corners.Field.Corners.Inverted -> []
                            in
                            let (env, shortcut) =
                              Typing_corners.For_test.corners_for
                                env
                                ~depended_on
                                ~live_sub
                                ~live_super
                                ~sub:sub_row
                                ~super:super_row
                                label
                                key
                                given
                                r
                            in
                            let shortcut = expect_ok shortcut in
                            let shortcut_corners =
                              match shortcut with
                              | Typing_corners.Field.Corners.Values cs -> cs
                              | Typing_corners.Field.Corners.Inverted -> []
                            in
                            let situation =
                              Printf.sprintf
                                "T1 as %s, %s <: %s at %s, %s"
                                (show_row bound_row)
                                (describe sub_shape)
                                (describe super_shape)
                                (describe_label label)
                                (if depended then
                                  "something depends on T1"
                                else
                                  "nothing depends on T1")
                            in
                            List.iter shortcut_corners ~f:(fun c ->
                                assert_bool
                                  (Printf.sprintf
                                     "With %s: the shortcut keeps a corner that is not one of the real ones."
                                     situation)
                                  (List.exists all_corners ~f:(fun c' ->
                                       Bool.equal c.sft_optional c'.sft_optional
                                       && Typing_defs.ty_equal
                                            c.sft_ty
                                            c'.sft_ty)));
                            let holds corners =
                              List.for_all corners ~f:(fun c ->
                                  obligation_holds_at
                                    env
                                    ~sub_row
                                    ~super_row
                                    ~label
                                    ~key
                                    ~given
                                    c)
                            in
                            assert_bool
                              (Printf.sprintf
                                 "With %s: checking the %d shortcut corners accepts, but checking all %d rejects. The shortcut lets a wrong program through."
                                 situation
                                 (List.length shortcut_corners)
                                 (List.length all_corners))
                              ((not (holds shortcut_corners))
                              || holds all_corners)
                          end))))))

(* == What the order does when the dependencies form a cycle ================
 *
 * `where T1 = T2` makes each parameter depend on the other, and no order can
 * give both a value before the other needs it. The code drops one of the
 * dependencies and carries on. There is no right order to compare against, but
 * two things must still hold, and neither was checked before.
 *
 * It must be total: every parameter still gets a place, or one would be left
 * without a value. And it must be deterministic: the same bounds must give the
 * same order every time, or which programs are accepted could vary between runs
 * of the same compiler. *)

let cyclic_env () =
  let env = dummy_env () in
  let env = Env.add_upper_bound env "T1" (splat [tgeneric "T2"]) in
  let env = Env.add_upper_bound env "T2" (splat [tgeneric "T1"]) in
  env

let a_cycle_still_orders_every_parameter_the_same_way _ =
  let env = cyclic_env () in
  let start = Typing_corners.Splat_elem.Set.singleton (tgeneric "T1") in
  let reached = Typing_corners.For_test.closure env start r in
  let run () =
    let cache = Typing_corners.Cache.create () in
    Typing_corners.topo cache env start r
  in
  let first = run () in
  (* Total: nothing dropped. *)
  assert_bool
    (Printf.sprintf
       "With T1 and T2 each depending on the other, the order is %s but the parameters needing values are %s. One would be left without a value."
       (show (List.map first ~f:param_name))
       (show (names (Typing_corners.Splat_elem.Set.elements reached))))
    (Int.equal
       (List.length first)
       (Typing_corners.Splat_elem.Set.cardinal reached)
    && Typing_corners.Splat_elem.Set.equal
         (Typing_corners.Splat_elem.Set.of_list first)
         reached);
  (* Deterministic: same bounds, same order, every time. *)
  List.iter (List.range 0 5) ~f:(fun attempt ->
      let again = run () in
      assert_bool
        (Printf.sprintf
           "With T1 and T2 each depending on the other, the order was %s and then %s on attempt %d. Which programs are accepted would depend on the run."
           (show (List.map first ~f:param_name))
           (show (List.map again ~f:param_name))
           attempt)
        (List.equal
           (fun left right ->
             Int.equal 0 (Typing_corners.Splat_elem.compare left right))
           first
           again))

let component_names component =
  Typing_corners.Component.members component
  |> Typing_corners.Splat_elem.Set.elements
  |> names

let sccs_preserve_partitions_and_dependency_order _ =
  let a = tgeneric "A"
  and b = tgeneric "B"
  and c = tgeneric "C"
  and equal_1 = tgeneric "Equal1"
  and equal_2 = tgeneric "Equal2" in
  let env = Env.add_upper_bound (dummy_env ()) "A" (splat [b]) in
  let env = Env.add_upper_bound env "B" (splat [c]) in
  let env = Env.add_upper_bound env "C" (splat [b]) in
  let env = Env.add_upper_bound env "Equal1" open_shape in
  let env = Env.add_upper_bound env "Equal1" equal_2 in
  let env = Env.add_upper_bound env "Equal2" open_shape in
  let env = Env.add_upper_bound env "Equal2" equal_1 in
  let roots = Typing_corners.Splat_elem.Set.of_list [a; equal_1] in
  let analysis = Typing_corners.analyze_dependencies env roots r in
  let components = Typing_corners.Analysis.components analysis in
  let partitions =
    List.map components ~f:(fun component -> show (component_names component))
    |> List.sort ~compare:String.compare
  in
  assert_equal
    ~printer:(String.concat ~sep:";")
    ["A"; "B,C"; "Equal1,Equal2"]
    partitions;
  let component_with name =
    Option.value_exn
      (List.find components ~f:(fun component ->
           List.mem (component_names component) name ~equal:String.equal))
  in
  (match component_with "A" with
  | Typing_corners.Component.Acyclic _ -> ()
  | Typing_corners.Component.Proven_equal _
  | Typing_corners.Component.Unsupported_cycle _ ->
    assert_failure "A should form an acyclic component");
  let unsupported_cycle =
    match component_with "B" with
    | Typing_corners.Component.Unsupported_cycle info -> info
    | Typing_corners.Component.Acyclic _
    | Typing_corners.Component.Proven_equal _ ->
      assert_failure "a nested-spread cycle must not be classified as equality"
  in
  let dependency_cycle =
    Typing_corners.Cycle_info.dependency_cycle unsupported_cycle
  in
  (match dependency_cycle with
  | [] -> assert_failure "an unsupported SCC must retain a witness cycle"
  | first :: rest ->
    let first_source = Typing_corners.Dependency.source first in
    let rec check_path expected_source = function
      | [] ->
        assert_equal
          0
          (Typing_corners.Splat_elem.compare expected_source first_source)
      | dependency :: rest ->
        assert_equal
          0
          (Typing_corners.Splat_elem.compare
             expected_source
             (Typing_corners.Dependency.source dependency));
        check_path (Typing_corners.Dependency.target dependency) rest
    in
    check_path (Typing_corners.Dependency.target first) rest);
  assert_bool
    "the witness retains the nested-spread edge classification"
    (List.for_all dependency_cycle ~f:(fun dependency ->
         match Typing_corners.Dependency.kind dependency with
         | Typing_corners.Dependency.Nested_upper -> true
         | Typing_corners.Dependency.Direct_upper
         | Typing_corners.Dependency.Indirect_upper
         | Typing_corners.Dependency.Nested_lower ->
           false));
  (match component_with "Equal1" with
  | Typing_corners.Component.Proven_equal _ -> ()
  | Typing_corners.Component.Acyclic _
  | Typing_corners.Component.Unsupported_cycle _ ->
    assert_failure "reciprocal direct bounds should prove equality");
  let component_index member =
    match
      List.findi components ~f:(fun _ component ->
          Typing_corners.Splat_elem.Set.mem
            member
            (Typing_corners.Component.members component))
    with
    | Some (index, _) -> index
    | None -> assert_failure "dependency analysis omitted a reachable member"
  in
  List.iter
    (Typing_corners.Analysis.dependencies analysis)
    ~f:(fun dependency ->
      let source = Typing_corners.Dependency.source dependency in
      let target = Typing_corners.Dependency.target dependency in
      let source_component = component_index source in
      let target_component = component_index target in
      if not (Int.equal source_component target_component) then
        assert_bool
          (Printf.sprintf
             "%s's component occurs before the component it depends on (%s)"
             (param_name source)
             (param_name target))
          (target_component < source_component))

let dependencies_retain_their_kind_and_position _ =
  let dependency_pos =
    Pos.make_from_lnum_bol_offset
      ~pos_file:(Relative_path.from_root ~suffix:"dependency.php")
      ~pos_start:(3, 8, 10)
      ~pos_end:(3, 8, 16)
  in
  let target = mk (Reason.witness dependency_pos, Tgeneric "Target") in
  let env = Env.add_upper_bound (dummy_env ()) "Direct" target in
  let env = Env.add_upper_bound env "NestedUpper" (splat [target]) in
  let env = Env.add_lower_bound env "NestedLower" (splat [target]) in
  let roots =
    Typing_corners.Splat_elem.Set.of_list
      [tgeneric "Direct"; tgeneric "NestedUpper"; tgeneric "NestedLower"]
  in
  let dependencies =
    Typing_corners.analyze_dependencies env roots r
    |> Typing_corners.Analysis.dependencies
  in
  let dependency_from source =
    Option.value_exn
      (List.find dependencies ~f:(fun dependency ->
           String.equal
             source
             (param_name (Typing_corners.Dependency.source dependency))))
  in
  let check source expected_kind =
    let dependency = dependency_from source in
    assert_equal expected_kind (Typing_corners.Dependency.kind dependency);
    assert_equal
      (Pos_or_decl.of_raw_pos dependency_pos)
      (Typing_corners.Dependency.position dependency)
  in
  check "Direct" Typing_corners.Dependency.Direct_upper;
  check "NestedUpper" Typing_corners.Dependency.Nested_upper;
  check "NestedLower" Typing_corners.Dependency.Nested_lower

let classified_dependencies_match_existing_dependencies _ =
  List.iter placements ~f:(fun place ->
      List.iter [Upper_bound_on_t2; Lower_bound_on_t1] ~f:(fun written_as ->
          let env =
            set_up_bounds (dummy_env ()) written_as ~place:place.build
          in
          let roots =
            Typing_corners.Splat_elem.Set.of_list [tgeneric "T1"; tgeneric "T2"]
          in
          let analysis = Typing_corners.analyze_dependencies env roots r in
          let dependencies = Typing_corners.Analysis.dependencies analysis in
          let reached = Typing_corners.closure env roots r in
          Typing_corners.Splat_elem.Set.iter
            (fun source ->
              let expected =
                Typing_corners.type_params_in_bounds env source r
                |> Typing_corners.Splat_elem.Set.of_list
              in
              let actual =
                List.filter_map dependencies ~f:(fun dependency ->
                    if
                      Int.equal
                        (Typing_corners.Splat_elem.compare
                           source
                           (Typing_corners.Dependency.source dependency))
                        0
                    then
                      Some (Typing_corners.Dependency.target dependency)
                    else
                      None)
                |> Typing_corners.Splat_elem.Set.of_list
              in
              assert_bool
                (Printf.sprintf
                   "classified dependencies differ for %s in %s written as %s"
                   (param_name source)
                   place.descr
                   (describe_written_as written_as))
                (Typing_corners.Splat_elem.Set.equal expected actual))
            reached))

let scc_analysis_visits_each_node_and_edge_once _ =
  let count = 18 in
  let params =
    List.map (List.range 0 count) ~f:(fun index ->
        tgeneric (Printf.sprintf "Dense%02d" index))
  in
  let same left right =
    Int.equal (Typing_corners.Splat_elem.compare left right) 0
  in
  let env =
    List.fold params ~init:(dummy_env ()) ~f:(fun env source ->
        let dependencies =
          List.filter params ~f:(fun target -> not (same source target))
        in
        Env.add_upper_bound env (param_name source) (splat dependencies))
  in
  let roots = Typing_corners.Splat_elem.Set.singleton (List.hd_exn params) in
  let analysis = Typing_corners.analyze_dependencies env roots r in
  assert_equal count (Typing_corners.Analysis.node_visits analysis);
  assert_equal
    (count * (count - 1))
    (Typing_corners.Analysis.edge_visits analysis);
  match Typing_corners.Analysis.components analysis with
  | [Typing_corners.Component.Unsupported_cycle info] ->
    assert_equal
      count
      (Typing_corners.Splat_elem.Set.cardinal
         (Typing_corners.Cycle_info.members info))
  | _ -> assert_failure "the dense graph should form one unsupported SCC"

(* == No label a check must cover is left out =================================
 *
 * The checks above all concern one label at a time, and the code they exercise
 * cannot behave differently because of another label: it is handed the one
 * label and never looks at the others. The functions that DO span labels are
 * the ones deciding which labels a comparison must cover, and those get their
 * own check here, over rows carrying several fields at once.
 *
 * Leaving a label out is the worst kind of mistake available to them: the
 * obligation for that field is never checked at all, so a wrong program is
 * accepted rather than a right one rejected. *)

let multi_label_rows =
  let f name ty optional = (name, { sft_optional = optional; sft_ty = ty }) in
  let row fields ~open_ =
    {
      s_origin = Missing_origin;
      s_unknown_value =
        (if open_ then
          MakeType.mixed r
        else
          MakeType.nothing r);
      s_fields =
        List.fold fields ~init:TShapeMap.empty ~f:(fun acc (n, fd) ->
            TShapeMap.add
              (TShapeField.make (TSFlit_str (Pos_or_decl.none, n)))
              fd
              acc);
    }
  in
  List.concat_map [false; true] ~f:(fun open_ ->
      [
        row [] ~open_;
        row [f "a" (MakeType.int r) false] ~open_;
        row [f "b" (MakeType.bool r) true] ~open_;
        row [f "a" (MakeType.int r) false; f "b" (MakeType.bool r) true] ~open_;
        row
          [
            f "a" (MakeType.int r) false;
            f "b" (MakeType.bool r) false;
            f "c" (MakeType.int r) true;
          ]
          ~open_;
      ])

let every_label_that_matters_is_covered _ =
  let key = tgeneric "T1" in
  List.iter multi_label_rows ~f:(fun bound_row ->
      let env =
        Env.add_upper_bound
          (dummy_env ())
          "T1"
          (mk (r, Tshape (Shape_simple bound_row)))
      in
      List.iter multi_label_rows ~f:(fun sub ->
          List.iter multi_label_rows ~f:(fun super ->
              (* The parameter sits in the super, so its bound's labels matter
                 as well as the two rows' own. *)
              let sub_shape = Shape_simple sub in
              let super_shape =
                Shape_splat
                  { ss_elems = [key; mk (r, Tshape (Shape_simple super))] }
              in
              let (env, sub_row) = normalize_row env sub_shape in
              let (env, super_row) = normalize_row env super_shape in
              let covered =
                let cache = Typing_corners.Cache.create () in
                Typing_corners.subrow_labels
                  cache
                  env
                  ~sub:sub_row
                  ~super:super_row
                  r
              in
              (* Worked out here rather than by calling the functions being
                 checked. subrow_label_set IS the union of those, so using them
                 to say what the answer should be would pass whatever they
                 returned. *)
              let rec labels_of_row row =
                match row with
                | Shape_simple { s_fields; _ } ->
                  TShapeMap.fold
                    (fun k _ acc -> TShapeSet.add k acc)
                    s_fields
                    TShapeSet.empty
                | Shape_splat { ss_elems } ->
                  List.fold ss_elems ~init:TShapeSet.empty ~f:(fun acc ty ->
                      match get_node ty with
                      | Tshape st -> TShapeSet.union acc (labels_of_row st)
                      | _ -> acc)
              in
              let labels_of_normalized_row row =
                Typing_shape_normalize.Row.fold
                  row
                  ~bottom:(fun () -> TShapeSet.empty)
                  ~simple:(fun shape -> labels_of_row (Shape_simple shape))
                  ~elements:(fun elements ->
                    List.fold
                      elements
                      ~init:TShapeSet.empty
                      ~f:(fun acc element ->
                        match
                          get_node
                            (Typing_shape_normalize.Row.Element.ty element)
                        with
                        | Tshape shape ->
                          TShapeSet.union acc (labels_of_row shape)
                        | _ -> acc))
              in
              let labels_of_bound param =
                match
                  Typing_corners.bound_shape_upper
                    env
                    param
                    Typing_corners.Splat_elem.Map.empty
                    r
                with
                | (_, Typing_corners.Upper_shapes shapes) ->
                  List.fold shapes ~init:TShapeSet.empty ~f:(fun acc row ->
                      TShapeSet.union acc (labels_of_normalized_row row))
                | (_, Typing_corners.Upper_bottom)
                | (_, Typing_corners.Upper_unconstrained) ->
                  TShapeSet.empty
              in
              let must_cover =
                TShapeSet.union
                  (TShapeSet.union
                     (labels_of_row sub_shape)
                     (labels_of_row super_shape))
                  (labels_of_bound key)
              in
              TShapeSet.iter
                (fun label ->
                  assert_bool
                    (Printf.sprintf
                       "Field %s is mentioned by one of the rows or by T1's bound, but is not among the labels the comparison covers. Its obligation is never checked, so a wrong program would be accepted."
                       (TShapeField.name label))
                    (List.exists covered ~f:(fun l ->
                         match l with
                         | Some l -> TShapeField.equal l label
                         | None -> false)))
                must_cover;
              (* And the unnamed fields are always covered. *)
              assert_bool
                "The unnamed fields are not among the labels the comparison covers."
                (List.exists covered ~f:Option.is_none))))

let nominal_newtype_keys_ignore_stored_bounds _ =
  let rec nested_newtype depth =
    if Int.equal depth 0 then
      mk (r, Tnewtype ("N0", [], MakeType.mixed r))
    else
      let child = nested_newtype (depth - 1) in
      mk (r, Tnewtype (Printf.sprintf "N%d" depth, [], splat [child; child]))
  in
  let left = nested_newtype 18 in
  let right = nested_newtype 18 in
  let different_stored_bound =
    mk (r, Tnewtype ("N18", [], MakeType.string r))
  in
  let keys =
    Typing_corners.Splat_elem.Set.of_list [left; right; different_stored_bound]
  in
  assert_equal 1 (Typing_corners.Splat_elem.Set.cardinal keys);
  let int_arg = mk (r, Tnewtype ("N", [MakeType.int r], MakeType.mixed r)) in
  let bool_arg = mk (r, Tnewtype ("N", [MakeType.bool r], MakeType.mixed r)) in
  assert_bool
    "newtype arguments are part of spread-element identity"
    (not (Int.equal 0 (Typing_corners.Splat_elem.compare int_arg bool_arg)));
  let nested_arg stored_bound = mk (r, Tnewtype ("Inner", [], stored_bound)) in
  let outer arg = mk (r, Tnewtype ("Outer", [arg], MakeType.mixed r)) in
  assert_equal
    0
    (Typing_corners.Splat_elem.compare
       (outer (nested_arg (MakeType.int r)))
       (outer (nested_arg (MakeType.bool r))));
  let opaque_reason =
    Reason.opaque_type_from_module (Pos_or_decl.none, "M", r)
  in
  let opaque = mk (opaque_reason, get_node left) in
  assert_bool
    "module opacity is part of spread-element identity"
    (not (Int.equal 0 (Typing_corners.Splat_elem.compare left opaque)))

let cached_nested_bounds_cover_every_corner _ =
  let depth = 20 in
  let name i = Printf.sprintf "T%d" i in
  let generic i = tgeneric (name i) in
  let add_bounds env name bound =
    let env = Env.add_upper_bound env name bound in
    Env.add_lower_bound env name bound
  in
  let env =
    add_bounds
      (dummy_env ())
      (name depth)
      (simple_shape [("leaf", MakeType.int r)] ~open_:false)
  in
  let env =
    List.fold_right (List.range 0 depth) ~init:env ~f:(fun i env ->
        let left = generic (i + 1) in
        let right = generic (i + 1) in
        add_bounds env (name i) (splat [left; right]))
  in
  let (env, row) = normalize_row env (Shape_splat { ss_elems = [generic 0] }) in
  let cache = Typing_corners.Cache.create () in
  let labels = Typing_corners.subrow_labels cache env ~sub:row ~super:row r in
  assert_equal 2 (List.length labels);
  List.iter labels ~f:(fun label ->
      let (_env, valid) =
        Typing_corners.check_subrow_corners
          cache
          env
          ~sub:row
          ~super:row
          label
          r
          ~init:(fun env -> (env, true))
          ~conj:(fun (env, left) next ->
            let (env, right) = next env in
            (env, left && right))
          ~f:(fun env ~sub ~super ->
            ( env,
              Bool.equal sub.sft_optional super.sft_optional
              && Int.equal
                   0
                   (compare_locl_ty
                      ?normalize_lists:None
                      sub.sft_ty
                      super.sft_ty) ))
      in
      let valid = valid |> expect_ok |> expect_computed in
      assert_bool "identical rows must agree at every cached corner" valid)

let cache_does_not_reuse_results_from_another_env _ =
  let key = tgeneric "T" in
  let env =
    Env.add_upper_bound
      (dummy_env ())
      "T"
      (simple_shape [("first", MakeType.int r)] ~open_:false)
  in
  let (env, row) = normalize_row env (Shape_splat { ss_elems = [key] }) in
  let cache = Typing_corners.Cache.create () in
  let first = Typing_corners.subrow_labels cache env ~sub:row ~super:row r in
  let env =
    Env.add_upper_bound
      (dummy_env ())
      "T"
      (simple_shape [("second", MakeType.int r)] ~open_:false)
  in
  let (env, row) = normalize_row env (Shape_splat { ss_elems = [key] }) in
  let second = Typing_corners.subrow_labels cache env ~sub:row ~super:row r in
  let contains labels name =
    List.exists labels ~f:(function
        | Some label -> String.equal name (TShapeField.name label)
        | None -> false)
  in
  assert_bool
    "the first environment's label should be present"
    (contains first "first");
  assert_bool
    "the second environment's label should be present"
    (contains second "second");
  assert_bool
    "the first environment's cached label must not leak into the second"
    (not (contains second "first"))

let cyclic_bound_projection_requires_an_assignment _ =
  let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
  let env = Env.add_upper_bound (dummy_env ()) "T1" (splat [t2]) in
  let env = Env.add_upper_bound env "T2" (splat [t1]) in
  let label = Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))) in
  let (_env, bounds) =
    Typing_corners.field_bounds
      env
      t1
      label
      Typing_corners.Splat_elem.Map.empty
      r
  in
  match bounds with
  | Error missing ->
    assert_equal
      0
      (Typing_corners.Splat_elem.compare
         t2
         (Typing_corners.Missing_assignment.element missing))
  | Ok _ -> assert_failure "an unassigned cyclic spread must not project"

let unsupported_cycle_is_reported_before_projection _ =
  let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
  let env = Env.add_upper_bound (dummy_env ()) "T1" (splat [t2]) in
  let env = Env.add_upper_bound env "T2" (splat [t1]) in
  let (env, row) = normalize_row env (Shape_splat { ss_elems = [t1] }) in
  let projections = ref 0 in
  let (_env, result) =
    Typing_corners.check_subrow_corners
      (Typing_corners.Cache.create ())
      env
      ~sub:row
      ~super:row
      (Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))))
      r
      ~init:(fun env -> (env, true))
      ~conj:(fun (env, left) next ->
        let (env, right) = next env in
        (env, left && right))
      ~f:(fun env ~sub:_ ~super:_ ->
        Int.incr projections;
        (env, true))
  in
  (match expect_ok result with
  | Typing_corners.Unsupported_cycle info ->
    assert_equal
      2
      (Typing_corners.Splat_elem.Set.cardinal
         (Typing_corners.Cycle_info.members info))
  | Typing_corners.Computed _ ->
    assert_failure "an unsupported cycle must not produce a proposition"
  | Typing_corners.Empty ->
    assert_failure "an unsupported cycle must not be treated as uninhabited");
  assert_equal 0 !projections

let unsupported_cycle_is_not_an_empty_assignment_set _ =
  let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
  let env = Env.add_upper_bound (dummy_env ()) "T1" (splat [t2]) in
  let env = Env.add_upper_bound env "T2" (splat [t1]) in
  let roots = Typing_corners.Splat_elem.Set.singleton t1 in
  let (_env, result) =
    Typing_corners.corner_assignments
      (Typing_corners.Cache.create ())
      env
      roots
      (Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))))
      r
  in
  match expect_ok result with
  | Typing_corners.Unsupported_cycle _ -> ()
  | Typing_corners.Computed _ ->
    assert_failure "an unsupported cycle must not produce assignments"
  | Typing_corners.Empty ->
    assert_failure "an unsupported cycle must not be treated as uninhabited"

let inconsistent_exact_bounds_are_empty _ =
  let key = tgeneric "T" in
  let lower = simple_shape [] ~open_:false in
  let upper = simple_shape [("x", MakeType.int r)] ~open_:false in
  let env = Env.add_lower_bound (dummy_env ()) "T" lower in
  let env = Env.add_upper_bound env "T" upper in
  let (_env, result) =
    Typing_corners.corner_assignments
      (Typing_corners.Cache.create ())
      env
      (Typing_corners.Splat_elem.Set.singleton key)
      (Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))))
      r
  in
  match expect_ok result with
  | Typing_corners.Empty -> ()
  | Typing_corners.Computed _ ->
    assert_failure "inconsistent exact bounds must have no assignments"
  | Typing_corners.Unsupported_cycle _ ->
    assert_failure "acyclic inconsistent bounds are not a cycle"

let proven_equal_parameters_share_each_assignment _ =
  let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
  let env = Env.add_upper_bound (dummy_env ()) "T1" t2 in
  let env = Env.add_upper_bound env "T1" open_shape in
  let env = Env.add_upper_bound env "T2" t1 in
  let env = Env.add_upper_bound env "T2" open_shape in
  let (_env, result) =
    Typing_corners.corner_assignments
      (Typing_corners.Cache.create ())
      env
      (Typing_corners.Splat_elem.Set.singleton t1)
      (Some (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x"))))
      r
  in
  let assignments = result |> expect_ok |> expect_computed in
  assert_bool
    "equal parameters should have at least one corner"
    (not (List.is_empty assignments));
  List.iter assignments ~f:(fun assignment ->
      let left = Typing_corners.Splat_elem.Map.find t1 assignment in
      let right = Typing_corners.Splat_elem.Map.find t2 assignment in
      assert_equal left.sft_optional right.sft_optional;
      assert_equal
        0
        (compare_locl_ty ?normalize_lists:None left.sft_ty right.sft_ty))

let unsupported_cycles_widen_reads_and_preserve_right_masking _ =
  let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
  let env = Env.add_upper_bound (dummy_env ()) "T1" (splat [t2]) in
  let env = Env.add_upper_bound env "T2" (splat [t1]) in
  let explicit = simple_shape [("x", MakeType.int r)] ~open_:false in
  let (env, result) = Typing_corners.resolve_for_read env r [t1; explicit] in
  match get_node (expect_ok result) with
  | Tshape (Shape_simple { s_fields; s_unknown_value; _ }) ->
    let x =
      Option.value_exn
        (TShapeMap.find_opt
           (TShapeField.make (TSFlit_str (Pos_or_decl.none, "x")))
           s_fields)
    in
    assert_bool "the explicit right field stays required" (not x.sft_optional);
    assert_bool
      "the explicit right field keeps its type"
      (Typing_subtype.is_sub_type env x.sft_ty (MakeType.int r)
      && Typing_subtype.is_sub_type env (MakeType.int r) x.sft_ty);
    assert_bool
      "the cyclic unknown tail is widened to mixed"
      (Typing_utils.is_mixed env s_unknown_value)
  | _ -> assert_failure "read resolution must produce a simple shape"

let distributed_upper_intersection_exposes_all_rows _ =
  let key = tgeneric "T" in
  let members =
    [tgeneric "U"; simple_shape [("x", MakeType.int r)] ~open_:true]
  in
  let intersection = splat [mk (r, Tintersection members)] in
  let env = Env.add_upper_bound (dummy_env ()) "T" intersection in
  match
    Typing_corners.bound_shape_upper
      env
      key
      Typing_corners.Splat_elem.Map.empty
      r
  with
  | (_, Typing_corners.Upper_shapes rows) -> assert_equal 2 (List.length rows)
  | (_, Typing_corners.Upper_bottom)
  | (_, Typing_corners.Upper_unconstrained) ->
    assert_failure "an upper-bound intersection must expose both rows"

let distributed_intersection_rows_survive_outer_bound_expansion _ =
  let key = tgeneric "T" in
  let members =
    [tgeneric "U"; simple_shape [("x", MakeType.int r)] ~open_:true]
  in
  let other = simple_shape [("y", MakeType.bool r)] ~open_:true in
  let distributed_intersection = splat [mk (r, Tintersection members)] in
  let upper_bound = mk (r, Tintersection [distributed_intersection; other]) in
  let upper_env = Env.add_upper_bound (dummy_env ()) "T" upper_bound in
  match
    Typing_corners.bound_shape_upper
      upper_env
      key
      Typing_corners.Splat_elem.Map.empty
      r
  with
  | (_, Typing_corners.Upper_shapes rows) -> assert_equal 3 (List.length rows)
  | (_, Typing_corners.Upper_bottom)
  | (_, Typing_corners.Upper_unconstrained) ->
    assert_failure "outer upper-bound expansion dropped distributed rows"

let distributed_lower_union_exposes_all_rows _ =
  let key = tgeneric "T" in
  let members =
    [tgeneric "U"; simple_shape [("x", MakeType.int r)] ~open_:true]
  in
  let union = splat [mk (r, Tunion members)] in
  let env = Env.add_lower_bound (dummy_env ()) "T" union in
  match
    Typing_corners.bound_shape_lower
      env
      key
      Typing_corners.Splat_elem.Map.empty
      r
  with
  | (_, Typing_corners.Lower_shapes rows) -> assert_equal 2 (List.length rows)
  | (_, Typing_corners.Lower_bottom) ->
    assert_failure "a lower-bound union must expose both rows"

let distributed_union_rows_survive_outer_bound_expansion _ =
  let key = tgeneric "T" in
  let members =
    [tgeneric "U"; simple_shape [("x", MakeType.int r)] ~open_:true]
  in
  let other = simple_shape [("y", MakeType.bool r)] ~open_:true in
  let distributed_union = splat [mk (r, Tunion members)] in
  let lower_bound = mk (r, Tunion [distributed_union; other]) in
  let lower_env = Env.add_lower_bound (dummy_env ()) "T" lower_bound in
  match
    Typing_corners.bound_shape_lower
      lower_env
      key
      Typing_corners.Splat_elem.Map.empty
      r
  with
  | (_, Typing_corners.Lower_shapes rows) -> assert_equal 3 (List.length rows)
  | (_, Typing_corners.Lower_bottom) ->
    assert_failure "outer lower-bound expansion dropped distributed rows"

let bottom_upper_view_beside_shape_is_discarded _ =
  let key = tgeneric "T" in
  let upper =
    mk
      ( r,
        Tintersection
          [
            splat [MakeType.nothing r];
            simple_shape [("x", MakeType.int r)] ~open_:true;
          ] )
  in
  let env = Env.add_upper_bound (dummy_env ()) "T" upper in
  match
    Typing_corners.bound_shape_upper
      env
      key
      Typing_corners.Splat_elem.Map.empty
      r
  with
  | (_, Typing_corners.Upper_shapes [_]) -> ()
  | (_, Typing_corners.Upper_shapes rows) ->
    assert_failure
      (Printf.sprintf "expected one non-bottom row, got %d" (List.length rows))
  | (_, Typing_corners.Upper_bottom) ->
    assert_failure "the bottom view was not discarded"
  | (_, Typing_corners.Upper_unconstrained) ->
    assert_failure "expected the non-bottom shape view"

let supportdyn_exact_open_bound_projects_exactly _ =
  let key = tgeneric "T" in
  let bound = MakeType.supportdyn r open_shape in
  let env = Env.add_lower_bound (dummy_env ()) "T" bound in
  let env = Env.add_upper_bound env "T" bound in
  let (env, bounds) =
    Typing_corners.field_bounds
      env
      key
      None
      Typing_corners.Splat_elem.Map.empty
      r
  in
  let (lower, upper) = expect_ok bounds in
  assert_bool "the lower open-tail field should be optional" lower.sft_optional;
  assert_bool "the upper open-tail field should be optional" upper.sft_optional;
  assert_bool
    "the lower open-tail field should have type mixed"
    (Typing_utils.is_mixed env lower.sft_ty);
  assert_bool
    "the upper open-tail field should have type mixed"
    (Typing_utils.is_mixed env upper.sft_ty)

let () =
  "typing_corners"
  >::: [
         "every_parameter_in_a_bound_is_recorded"
         >:: every_parameter_in_a_bound_is_recorded;
         "a_spread_parameter_is_found" >:: a_spread_parameter_is_found;
         "a_spread_parameter_in_a_nested_intersection_is_found"
         >:: a_spread_parameter_in_a_nested_intersection_is_found;
         "a_field_type_is_not_a_dependency" >:: a_field_type_is_not_a_dependency;
         "following_bounds_reaches_everything"
         >:: following_bounds_reaches_everything;
         "the_order_gives_values_before_they_are_needed"
         >:: the_order_gives_values_before_they_are_needed;
         "independent_roots_keep_the_assignment_frontier_narrow"
         >:: independent_roots_keep_the_assignment_frontier_narrow;
         "a_cycle_still_orders_every_parameter_the_same_way"
         >:: a_cycle_still_orders_every_parameter_the_same_way;
         "sccs_preserve_partitions_and_dependency_order"
         >:: sccs_preserve_partitions_and_dependency_order;
         "dependencies_retain_their_kind_and_position"
         >:: dependencies_retain_their_kind_and_position;
         "classified_dependencies_match_existing_dependencies"
         >:: classified_dependencies_match_existing_dependencies;
         "scc_analysis_visits_each_node_and_edge_once"
         >:: scc_analysis_visits_each_node_and_edge_once;
         "every_label_that_matters_is_covered"
         >:: every_label_that_matters_is_covered;
         "nominal_newtype_keys_ignore_stored_bounds"
         >:: nominal_newtype_keys_ignore_stored_bounds;
         "cached_nested_bounds_cover_every_corner"
         >:: cached_nested_bounds_cover_every_corner;
         "cache_does_not_reuse_results_from_another_env"
         >:: cache_does_not_reuse_results_from_another_env;
         "cyclic_bound_projection_requires_an_assignment"
         >:: cyclic_bound_projection_requires_an_assignment;
         "unsupported_cycle_is_reported_before_projection"
         >:: unsupported_cycle_is_reported_before_projection;
         "unsupported_cycle_is_not_an_empty_assignment_set"
         >:: unsupported_cycle_is_not_an_empty_assignment_set;
         "inconsistent_exact_bounds_are_empty"
         >:: inconsistent_exact_bounds_are_empty;
         "proven_equal_parameters_share_each_assignment"
         >:: proven_equal_parameters_share_each_assignment;
         "unsupported_cycles_widen_reads_and_preserve_right_masking"
         >:: unsupported_cycles_widen_reads_and_preserve_right_masking;
         "distributed_upper_intersection_exposes_all_rows"
         >:: distributed_upper_intersection_exposes_all_rows;
         "distributed_intersection_rows_survive_outer_bound_expansion"
         >:: distributed_intersection_rows_survive_outer_bound_expansion;
         "distributed_lower_union_exposes_all_rows"
         >:: distributed_lower_union_exposes_all_rows;
         "distributed_union_rows_survive_outer_bound_expansion"
         >:: distributed_union_rows_survive_outer_bound_expansion;
         "bottom_upper_view_beside_shape_is_discarded"
         >:: bottom_upper_view_beside_shape_is_discarded;
         "supportdyn_exact_open_bound_projects_exactly"
         >:: supportdyn_exact_open_bound_projects_exactly;
         "a_field_is_hidden_exactly_when_something_hides_it"
         >:: a_field_is_hidden_exactly_when_something_hides_it;
         "checking_fewer_corners_accepts_no_more"
         >:: checking_fewer_corners_accepts_no_more;
       ]
  |> run_test_tt_main
