<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class C {
  public function foo():this {
    return (new D<int>(3)) as this;
  }
}
class D<T> extends C {
  public function __construct(public T $item) { }
}

function expectString(string $x):void { }

<<__EntryPoint>>
function testit():void {
  $d = new D("a");
  $x = $d->foo();
  expectString($x->item);
}
