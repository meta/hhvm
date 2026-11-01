<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function foo(inout int $_): void {
  $_ = "";
}

<<__EntryPoint>>
function my_main(): void {
  $x = 0;
  foo(inout $x); // boom
}
