<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

abstract class C1 {
  abstract const int X;
  public static function f(): int {
    $x = static::X;
    return $x;
  }
}

<<__EntryPoint>>
function main():void {
  C1::f();
}
