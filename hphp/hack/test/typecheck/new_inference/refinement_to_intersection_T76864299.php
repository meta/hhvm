<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

interface J {  }

function bar<T>(T $q, J $j): T {
  invariant($q is J, 'haha');

  $q = $q as J;
  return $j;
}

class C implements J {
  public function boo():void { }
}
class D implements J {
}

<<__EntryPoint>>
function testit():void {
  // Must have C <: T
  // Also have C <: J (otherwise bar would throw)
  // So does it follow that J <: T? NO!!
  $x = bar<C>(new C(), new D());
  // Crash!
  $x->boo();
}
