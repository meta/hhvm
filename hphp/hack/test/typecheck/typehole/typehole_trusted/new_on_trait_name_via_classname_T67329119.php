<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
class MyFooClass {}

trait FooTrait {
  require extends MyFooClass;
}

<<__EntryPoint>>
function bar(): void {
  $x = FooTrait::class;
  new_it($x);
}

function new_it(class<MyFooClass> $c): void {
  new $c(); // boom
}
