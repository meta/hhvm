<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class A {
  private function f(): void { echo "A::f\n"; }
}
class B extends A {
  public static function f(): void { echo "B::f\n"; }
}
class C extends B {
  public static function f(): void { echo "C::f\n"; }
}
<<__EntryPoint>> function t12279(): void {
  B::f();
}
