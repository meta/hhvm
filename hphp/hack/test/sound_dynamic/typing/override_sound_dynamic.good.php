<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class C {
  public function foo(int $_):void { }
  public function bar(): dynamic { return (\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>("A")); }
}
class D extends C {
  public function foo(dynamic $_):void { }
  public function bar(): vec<int> { return vec[]; }
}
