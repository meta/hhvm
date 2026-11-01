<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

abstract class MyClass {
  abstract public static function foo(): void;
}

trait MyTrait {
  require extends MyClass;
}

function takes_classname(classname<MyClass> $cn): void {
  $cn::foo();
}

<<__EntryPoint>>
function call_it(): void {
  // accepted by hh, error at runtime (calling abstract method)
  takes_classname(nameof MyTrait);
}
