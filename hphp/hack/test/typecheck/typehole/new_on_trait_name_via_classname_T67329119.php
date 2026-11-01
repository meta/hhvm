<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
class MyFooClass {}

trait FooTrait {
  require extends MyFooClass;
}

<<__EntryPoint>>
function bar(): void {
  $x = nameof FooTrait;
  new_it($x);
}

function new_it(classname<MyFooClass> $c): void {
  new $c(); // boom
}
