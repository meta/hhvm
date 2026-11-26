<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__SupportDynamicType>>
class C { }

<<__SupportDynamicType>>
function foo(int $_):bool {
  return false;
}

function expectFun((function(int):bool) $_):void { }

function test1(
  (function(dynamic):int) $f1,
  (function(mixed, dynamic):string) $f2,
  (function((string|dynamic),dynamic...):float) $f3,
  (function():void) $f4,
  (function():C) $f5):void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f1);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f2);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f3);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f4);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f5);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x ==> $x);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>((dynamic $d) ==> $d->foo());
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(foo<>);
  expectFun(foo<>);
}
