<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

abstract class AbstractBase {
  public abstract static function foo(): void;

  <<__NeedsConcrete>>
  public static function bar(): void {
    static::foo();
  }
}

<<__EntryPoint>>
function call_it(): void {
  AbstractBase::bar();
}
