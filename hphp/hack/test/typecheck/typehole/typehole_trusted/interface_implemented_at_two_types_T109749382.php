<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__UNSAFE_AllowMultipleInstantiations>>
interface I<T> {
  public function foo():T;
}

interface I1 extends I<string> { }
interface I2 extends I<int>, I1 { }

class C implements I2 {
  public function foo():int { return 3; }
}

function expectI1(I1 $x):string {
  return $x->foo();
}

<<__EntryPoint>>
function main():void {
  expectI1(new C());
}
