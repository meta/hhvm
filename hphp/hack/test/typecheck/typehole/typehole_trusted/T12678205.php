<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class B { }
class C {
  // These shape indices are
  // - not equal as far as static type-checking is concerned
  // - equal for runtime indexing :-(
  // So unsound
  <<__ShapeKey>>
  const string OfB = "whatevs";
  <<__ShapeKey>>
  const string OfC = "whatevs";
  public function Foo():void { echo 'Foo'; }
}
type ShapeTy = shape(C::OfB => B, C::OfC => C);
function DoIt(ShapeTy $x):void {
    $c = $x[C::OfC];
    $c->Foo();
  }

<<__EntryPoint>>
function BreakIt():void {
// Sensitive to order: this will not fail at runtime
// $s = shape(C::OfB => new B(), C::OfC => new C());
// But this will
  $s = shape(C::OfC => new C(), C::OfB => new B());
  DoIt($s);
}
