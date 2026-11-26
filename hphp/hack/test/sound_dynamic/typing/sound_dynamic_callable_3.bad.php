<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class C {
  <<__SupportDynamicType>>
  public function foo(vec<int> $x):int {
    \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, vec<int>>($x);
    return $x[0];
  }
}
