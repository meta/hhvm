(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Private members added or removed outside traits are classified as not
   affecting descendants. Their presence can nevertheless change inherited
   members: some private members are inherited, and others hide ancestor
   members. The resulting member fanout can omit affected descendants and
   their users. These cases exercise additions using completed edits. *)

(** Adding a private method with __TestsBypassVisibility makes Child's
    existing method an illegal shadow, but Child is not rechecked. *)
let test_add_private_method_with_tests_bypass_visibility () =
  let scenario =
    Scheduled_fanout.
      {
        (* The child initially has a private method with no inherited conflict. *)
        initial_files =
          [
            ("Parent.php", "<?hh\nclass ParentClass {}\n");
            ( "Child.php",
              {|<?hh
class ChildClass extends ParentClass {
  private function target(): void {}
}
|}
            );
          ];
        iterations =
          [
            {
              default_iteration with
              (* The new attribute makes this private method inherited, so
                 Child's existing method becomes an illegal shadow. *)
              before_type_check =
                [
                  Write_file
                    ( "Parent.php",
                      {|<?hh
class ParentClass {
  <<__TestsBypassVisibility>>
  private function target(): void {}
}
|}
                    );
                ];
              disk_needs_parsing = ["Parent.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* A fresh check reports Child's shadowing error. Incremental checking must
     discover the same error without a separate edit to Child. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F1: private member additions"
    ~expectation:
      (Diagnostics
         {
           incremental = [];
           full = [("Child.php", 4514, "Cannot override or shadow `target`")];
         })
    scenario;
  true

let empty_parent = "<?hh\nclass C {}\n"

(** Adding C's inherited __LSB private static property makes D's existing
    property an illegal override, but D is not rechecked. *)
let test_lsb_private_property_added () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. Establish a clean baseline: C has no members, and D declares
           its own static property. *)
        initial_files =
          [
            ("C.php", empty_parent);
            ( "D.php",
              {|<?hh
class D extends C {
  public static int $p = 1;
}
|} );
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Add an `__LSB` private static property to C. The diff
                 treats it as a private change, whose fanout only includes
                 dependents of C::$p. D.php never referenced C::$p. *)
              before_type_check =
                [
                  Write_file
                    ( "C.php",
                      {|<?hh
class C {
  <<__LSB>> private static int $p = 0;
}
|}
                    );
                ];
              disk_needs_parsing = ["C.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 3. A fresh check reports that D::$p overrides C's `__LSB` member. The
     incremental check does not recheck D.php and reports nothing. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F1: private member additions"
    ~expectation:
      (Diagnostics
         {
           incremental = [];
           full =
             [
               ( "D.php",
                 4284,
                 "Member `$p` may not override `__LSB` member of parent" );
               ("D.php", 12021, "Static property is overridden: `$p`");
             ];
         })
    scenario;
  true

(** Parent's new private method hides Grandparent's public method from Child.
    Parent's override error is reported, but the caller's missing-method error
    is missed. *)
let test_private_method_hides_public_ancestor () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ( "Grandparent.php",
              {|<?hh
class GrandparentClass {
  public function target(): void {}
}
|}
            );
            ( "Parent.php",
              "<?hh\nclass ParentClass extends GrandparentClass {}\n" );
            ("Child.php", "<?hh\nclass ChildClass extends ParentClass {}\n");
            ( "Use.php",
              {|<?hh
function use_child(ChildClass $child): void {
  $child->target();
}
|}
            );
          ];
        iterations =
          [
            {
              default_iteration with
              (* Parent now has an illegal private override. Its folded member
                 replaces Grandparent's, then gets filtered out of Child. *)
              before_type_check =
                [
                  Write_file
                    ( "Parent.php",
                      {|<?hh
class ParentClass extends GrandparentClass {
  private function target(): void {}
}
|}
                    );
                ];
              disk_needs_parsing = ["Parent.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* In addition to Parent's override error, the fresh check reports that
     Child has no target method. The incremental check must report both. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F1: private member additions"
    ~expectation:
      (Diagnostics
         {
           incremental =
             [("Parent.php", 4113, "This member visibility is: `private`")];
           full =
             [
               ("Use.php", 4053, "No instance method `target` in `ChildClass`");
               ("Parent.php", 4113, "This member visibility is: `private`");
             ];
         })
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "add_private_method_with_tests_bypass_visibility",
        test_add_private_method_with_tests_bypass_visibility );
      ("lsb_private_property_added", test_lsb_private_property_added);
      ( "private_method_hides_public_ancestor",
        test_private_method_hides_public_ancestor );
    ]
