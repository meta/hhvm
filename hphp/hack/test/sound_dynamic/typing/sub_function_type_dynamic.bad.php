<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class C { }
function test1(
  (function(int):int) $f1,
  (function(mixed, dynamic):mixed) $f2,
  (function():vec<mixed>) $f3,
  (function():C) $f4):void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f1);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f2);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f3);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f4);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>((int $x) ==> $x+1);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x ==> new C());
}
