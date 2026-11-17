<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class C {
  public function foo():void { }
}
class D {
}

function testit():void {
  $v = Vector{};
  $v->add(() ==> new C())->add(() ==> new D());
  // $v is assigned type Vector<function():C> !!
  $f = ($v[1])();
  $f->foo();
}
