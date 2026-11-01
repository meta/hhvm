<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

enum MyFoo: int as int {
  X = 1;
}

  function takes_enum(MyFoo $_): void {
}

function mixed_to_enum(mixed $m): void {
  takes_enum($m as MyFoo);
}

<<__EntryPoint>>
function my_main(): void {
  mixed_to_enum('1');
}
