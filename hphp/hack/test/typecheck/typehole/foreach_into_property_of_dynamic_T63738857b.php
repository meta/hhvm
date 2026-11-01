<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function my_example(darray<int, int> $items): void {
  $k = null as dynamic;
  $v = null;
  foreach ($items as $k->$v) {
  }
}

<<__EntryPoint>>
function main():void {
  my_example(dict[2 => 3]);
}
