<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class C {
  abstract const type Tc as mixed;
}
class D extends C {
  const type Tc = string;
}
class E extends C {
  const type Tc = int;
}
function expectString(string $x):void { }
function foo<T as E as D, Tx>(Tx $x):void where Tx = T::Tc {
  expectString($x);
}
function testit():void {
  foo(3);
}
