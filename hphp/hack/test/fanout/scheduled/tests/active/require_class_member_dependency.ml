(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* When checking a trait against an interface, eager member resolution can
   select a member from the trait's required class. That lookup does not record
   a dependency on the selected member. A member change can therefore alter the
   compatibility check without including the trait in its fanout. *)

let class_contents return_type =
  Printf.sprintf
    {|<?hh
final class C {
  use T;
  public function m(): %s {
    throw new Exception();
  }
}
|}
    return_type

(** Changing C::m's return type invalidates T's compatibility with I.
    The edit reports C's override error but misses T's interface error because
    the eager lookup did not record T's dependency on C::m. *)
let test_require_class_method_signature_change () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. Establish a clean baseline: C::m matches I::m, so neither T nor C
           has diagnostics. *)
        initial_files =
          [
            ("I.php", {|<?hh
interface I {
  public function m(): int;
}
|});
            ("T.php", {|<?hh
trait T implements I {
  require class C;
}
|});
            ("C.php", class_contents "int");
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Change only C::m's return type, so this is a member change
                 rather than a change requiring all class users to recheck. *)
              before_type_check =
                [Write_file ("C.php", class_contents "string")];
              disk_needs_parsing = ["C.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 3. A fresh check additionally reports that T's members are incompatible
     with I. The incremental check misses this error on T.php. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F11: required-class member dependencies"
    ~expectation:
      (Diagnostics
         {
           incremental =
             [
               ( "C.php",
                 4341,
                 "The method `m` is not compatible with the overridden method"
               );
             ];
           full =
             [
               ( "T.php",
                 4341,
                 "Some members in class `T` are incompatible with those declared in type `I`"
               );
               ( "C.php",
                 4341,
                 "The method `m` is not compatible with the overridden method"
               );
             ];
         })
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "require_class_method_signature_change",
        test_require_class_method_signature_change );
    ]
