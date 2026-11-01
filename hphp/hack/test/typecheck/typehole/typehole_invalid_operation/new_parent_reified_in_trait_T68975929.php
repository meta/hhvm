<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

trait T {
  require extends Foo<int>;
  public static function bar(): void { new parent(); }
}

class Foo<reify T> {}

class C extends Foo<int> { use T; }

<<__EntryPoint>>
function main(): void {
  C::bar();
}
