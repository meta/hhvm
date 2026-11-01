<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
interface I { }

function test_it(bool $b): void {
  $t = I::class;
  $x = new $t();
}

<<__EntryPoint>>
function main():void {
  test_it(true);
}
