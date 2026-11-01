<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

trait T {
  private static function foo(): void {}
  public static function bar(): void {
    T::foo();
  }
}

class C {
  use T;
}

<<__EntryPoint>>
function test_main(): void {
  C::bar();
}
