<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

trait BarTrait2 {
  abstract protected static function foo(): void;

  <<__NeedsConcrete>>
  public static function start(): void {
    static::foo();
  }
}

<<__EntryPoint>>
function crash_on_trait_call(): void {
  BarTrait2::start();
}
