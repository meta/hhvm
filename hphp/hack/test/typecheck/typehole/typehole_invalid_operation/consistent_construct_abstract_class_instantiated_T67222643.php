<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
abstract class FooA {
  abstract const type T;
  public function __construct(private string $a) {}
  <<__NeedsConcrete>>
  public static function create(string $a): FooA {
    return new static($a);
  }
}

<<__EntryPoint>>
function main():void {
  FooA::create('hello');
}
