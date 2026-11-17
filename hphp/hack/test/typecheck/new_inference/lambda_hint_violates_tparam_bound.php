<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class B<T> {
  public function breakit(T $ent): void { }
}
interface I {
  public function foo():void;
}
class C { }
class Wrap<T as I> extends B<T> {
  public function __construct((function(T):int) $f) { }
  public function breakit(T $ent): void { $ent->foo(); }
}

function testit(): void {
  $w = new Wrap((C $x) ==> 5);
  $w->breakit(new C());
}
