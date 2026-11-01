<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function foo(inout int $a): void {
  // list() is evaluated right-to-left in PHP/Hack, so $a is "foo".
  list($a, $a) = Pair {"foo", 1};
}

<<__EntryPoint>>
function my_main(): void {
  $x = 1;
  foo(inout $x); // boom
}
