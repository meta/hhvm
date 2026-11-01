<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function my_transform(vec<int> $v): KeyedContainer<arraykey, mixed> {
  return $v;
}

<<__EntryPoint>>
function use_it(): void {
  $x = vec[1];
  $y = my_transform($x);
  $y['foo']; // InvalidArgumentException
}
