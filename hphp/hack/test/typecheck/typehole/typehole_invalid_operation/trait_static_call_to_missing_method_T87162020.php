<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

interface MyInterface {
  static public function foo(): void;
}

trait MyTrait {
  require implements MyInterface;

  static public function bar(): void {
    self::foo();
  }
}

<<__EntryPoint>>
function mymain(): void {
  MyTrait::bar();
}
