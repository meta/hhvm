<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function bad_loop(): void {
  foreach (vec[1] as $outer) {
    $prev_item = null;

    foreach (vec[1, 2] as $item) {
      takes_nullable_str($prev_item);

      if ($prev_item === null) {
        $prev_item = $item;
      }
    }
  }
}

function takes_nullable_str(?string $x): void {
}


<<__EntryPoint>>
function main():void {
  bad_loop();
}
