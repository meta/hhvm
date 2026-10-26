(**
 * Copyright (c) 2019, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Class_diff
open Typing_deps

type changed_class = {
  name: string;
  diff: Class_diff.t;
  dep: Dep.t;
  descendant_deps: Typing_deps.DepSet.t;
}

let get_all_deps (ctx : Provider_context.t) (changed_class : changed_class) :
    DepSet.t =
  let mode = Provider_context.get_deps_mode ctx in
  Typing_deps.add_typing_deps mode changed_class.descendant_deps

let get_maximum_fanout
    (ctx : Provider_context.t) (changed_class : changed_class) : Fanout.t =
  {
    Fanout.changed = DepSet.singleton changed_class.dep;
    to_recheck = get_all_deps ctx changed_class;
    to_recheck_if_errors = DepSet.make ();
  }

let class_names_from_deps ~ctx ~get_classes_in_file deps =
  let filenames = Naming_provider.get_files ctx deps in
  Relative_path.Set.fold filenames ~init:S_set.empty ~f:(fun file acc ->
      S_set.fold
        (fun cid acc ->
          if DepSet.mem deps Dep.(make (Type cid)) then
            S_set.add cid acc
          else
            acc)
        (get_classes_in_file file)
        acc)

let include_fanout_of_dep (mode : Mode.t) (dep : Dep.t) (deps : DepSet.t) :
    DepSet.t =
  let fanout = Typing_deps.get_ideps_from_hash mode dep in
  DepSet.union fanout deps

let get_minor_change_fanout
    ~(ctx : Provider_context.t)
    (changed_class : changed_class)
    (member_diff : Class_diff.member_diff) : Fanout.t =
  let mode = Provider_context.get_deps_mode ctx in
  let acc = DepSet.singleton changed_class.dep in
  let { consts; typeconsts; props; sprops; methods; smethods; constructor } =
    member_diff
  in
  (* Recheck any file with a dependency on the provided member
     in the changed class and in each of its descendants,
     even if the member was overridden in a subclass. This deals with the case
     where adding, removing, or changing the abstract-ness of a member causes
     some subclass which inherits a member of that name from multiple parents
     to resolve the conflict in a different way than it did previously. *)
  let recheck_descendants_and_their_member_dependents (acc : DepSet.t) member :
      DepSet.t =
    DepSet.fold changed_class.descendant_deps ~init:acc ~f:(fun dep acc ->
        DepSet.add acc dep
        |> include_fanout_of_dep
             mode
             (Typing_deps.Dep.make_member_dep_from_type_dep dep member))
  in
  let add_member_fanout
      ~is_const
      (member : Dep.Member.t)
      (change : member_change)
      (acc : DepSet.t) =
    (* Consts and typeconsts have their types copied into descendant classes in
       folded decl (rather than being stored in a separate heap as methods and
       properties are). As a result, when using a const, we register a
       dependency only upon the class type at the use site. In contrast, if we
       use a method or property B::f which was inherited from A, we register a
       dependency both upon B::f and A::f, which is what allows us to avoid the
       more expensive use of recheck_descendants_and_their_member_dependents
       here. Since we don't register a dependency upon the const at its class of
       origin in this way, we must always take the more expensive path for
       consts. *)
    if
      is_const
      || Class_diff.method_or_property_change_affects_descendants change
    then
      recheck_descendants_and_their_member_dependents acc member
    else
      include_fanout_of_dep
        mode
        (Dep.make_member_dep_from_type_dep changed_class.dep member)
        acc
  in
  let add_member_fanouts ~is_const changes make_member acc =
    S_map.fold
      (fun name -> add_member_fanout ~is_const (make_member name))
      changes
      acc
  in
  let acc =
    S_map.fold
      (fun name change acc ->
        let acc =
          (* If a const changed in an enum type, we must recheck
             all switch statements which need to have a case for each variant
             (exhaustive switch statements add an AllMembers dependency).
             We don't bother to test whether the class is an enum type because
             non-enum classes will have no AllMembers dependents anyway.
             Note that if an existing constant's type changes, we also need to
             re-check exhaustive switches, since this can change the inhabitants
             of \HH\MemberOf<MyEnum, Foo>.
             We must include `Changed_inheritance` as well because changing
             abstractness and the constant's type at the same time yields
             `Changed_inheritance` instead of `Modified`. *)
          match change with
          | Added
          | Removed
          | Changed_inheritance
          | Modified _ ->
            recheck_descendants_and_their_member_dependents acc Dep.Member.all
          | Private_change_not_in_trait -> acc
        in
        add_member_fanout ~is_const:true (Dep.Member.const name) change acc)
      consts
      acc
  in
  let acc =
    acc
    |> add_member_fanouts ~is_const:true typeconsts Dep.Member.const
    |> add_member_fanouts ~is_const:false props Dep.Member.prop
    |> add_member_fanouts ~is_const:false sprops Dep.Member.sprop
    |> add_member_fanouts ~is_const:false methods Dep.Member.method_
    |> add_member_fanouts ~is_const:false smethods Dep.Member.smethod
  in
  let acc =
    Option.value_map constructor ~default:acc ~f:(fun change ->
        add_member_fanout ~is_const:false Dep.Member.constructor change acc)
  in
  {
    Fanout.changed = DepSet.singleton changed_class.dep;
    to_recheck = acc;
    to_recheck_if_errors = DepSet.make ();
  }

let get_fanout ~(ctx : Provider_context.t) (changed_class : changed_class) :
    Fanout.t =
  match changed_class.diff with
  | Major_change _change -> get_maximum_fanout ctx changed_class
  | Minor_change minor_change ->
    get_minor_change_fanout ~ctx changed_class minor_change

let direct_references_cardinal mode dep : int =
  Typing_deps.get_ideps_from_hash mode dep |> DepSet.cardinal

let descendants_cardinal descendant_deps : int =
  DepSet.cardinal descendant_deps - 1

let children_cardinal mode class_name : int =
  Typing_deps.get_ideps mode (Dep.Extends class_name) |> DepSet.cardinal

module Log : sig
  val log_class_fanout :
    Provider_context.t -> class_cnt:int -> changed_class -> Fanout.t -> unit

  val log_fanout :
    Provider_context.t ->
    'a list ->
    Fanout.t ->
    max_class_fanout_cardinal:int ->
    unit
end = struct
  let do_log ctx ~fanout_cardinal =
    Typechecker_options.log_fanout ~fanout_cardinal
    @@ Provider_context.get_tcopt ctx

  let max_class_cnt = 10

  let log_class_fanout
      ctx ~class_cnt (changed_class : changed_class) (fanout : Fanout.t) : unit
      =
    let { name; diff; dep; descendant_deps } = changed_class in
    let fanout_cardinal = Fanout.cardinal fanout in
    if class_cnt < max_class_cnt then
      Hh_logger.log
        "Fanout of %s (hash: %s) is %d deps: %s%s"
        name
        (Dep.to_hex_string dep)
        fanout_cardinal
        ( fanout.Fanout.to_recheck |> DepSet.elements |> fun l ->
          List.take l 5
          |> List.map ~f:Dep.to_hex_string
          |> String.concat ~sep:", " )
        (if fanout_cardinal > 5 then
          " (truncated)"
        else
          "")
    else if class_cnt = max_class_cnt then
      Hh_logger.log "(truncated)";
    if do_log ~fanout_cardinal ctx then
      let mode = Provider_context.get_deps_mode ctx in
      Hack_event_logger.Fanouts.log_class
        ~class_name:name
        ~class_diff:(Class_diff.show diff)
        ~fanout_cardinal
        ~class_diff_category:(Class_diff.to_category_json diff)
        ~direct_references_cardinal:(direct_references_cardinal mode dep)
        ~descendants_cardinal:(descendants_cardinal descendant_deps)
        ~children_cardinal:(children_cardinal mode name)

  let log_fanout
      ctx (changes : _ list) (fanout : Fanout.t) ~max_class_fanout_cardinal :
      unit =
    let fanout_cardinal = Fanout.cardinal fanout in
    if do_log ~fanout_cardinal ctx then
      Hack_event_logger.Fanouts.log
        ~changes_cardinal:(List.length changes)
        ~max_class_fanout_cardinal
        ~fanout_cardinal
end

let add_fanout
    class_cnt
    ~ctx
    ((fanout_acc, max_class_fanout_cardinal) : Fanout.t * int)
    (changed_class : changed_class) : Fanout.t * int =
  let fanout = get_fanout ~ctx changed_class in
  Log.log_class_fanout ctx ~class_cnt changed_class fanout;
  let fanout_acc = Fanout.union fanout_acc fanout in
  let max_class_fanout_cardinal =
    Int.max max_class_fanout_cardinal (Fanout.cardinal fanout)
  in
  (fanout_acc, max_class_fanout_cardinal)

let fanout_of_changes ~(ctx : Provider_context.t) (changes : changed_class list)
    : Fanout.t =
  let (fanout, max_class_fanout_cardinal) =
    List.foldi changes ~init:(Fanout.empty, 0) ~f:(add_fanout ~ctx)
  in
  Log.log_fanout ctx changes fanout ~max_class_fanout_cardinal;
  fanout
