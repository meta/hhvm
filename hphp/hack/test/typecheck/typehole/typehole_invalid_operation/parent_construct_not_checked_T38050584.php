<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class MyParent {}

class Child extends MyParent {
  public function __construct() {
    parent::__construct();
  }
}

<<__EntryPoint>>
function main(): void {
  new Child(); // boom
}
