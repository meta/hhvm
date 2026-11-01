<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

trait TraitFoo {
  protected int $x = 3;
}
trait TraitBar {
  protected int $x = 5;
}
class TraitTestClass {
  use TraitFoo;
  use TraitBar;
  public function output(): void {
    echo $this->x;
  }
}
<<__EntryPoint>>
function main():void {
  $x = new TraitTestClass();
  $x->output();
}
