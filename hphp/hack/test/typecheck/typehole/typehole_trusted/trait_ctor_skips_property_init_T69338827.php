<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class MyClass {
  public function __construct(public int $x): void {}
}

trait MyTrait {
  public function __construct(): void {}
}

class MyChild extends MyClass {
  use MyTrait;
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function my_main(): void {
  $v = new MyChild();
  takes_int($v->x); // passing null (uninitialised prop) as int
}
