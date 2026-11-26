<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.


<<__NoAutoDynamic>>
function expect_int(~int $_): void {}

<<__SupportDynamicType>>
class Box<T> {
}

<<__NoAutoDynamic>>
function e<T as supportdyn<mixed>>(~C<Box<T>> $param): ~T {
  return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(1);
}

<<__NoAutoDynamic>>
function get_C() : C<~Box<string>> { return new C<Box<string>>();}

<<__SupportDynamicType>>
class C<+T> {}

<<__NoAutoDynamic>>
function testit(): void {
  expect_int(e(get_C()));
}
