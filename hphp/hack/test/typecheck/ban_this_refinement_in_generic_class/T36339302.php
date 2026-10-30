<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class G<T> {
  public function __construct(public T $item) { }
  public function testit(mixed $m): G<T> {
    if ($m is this) {
      return $m;
    }
    throw new Exception("blah");
  }
}
function expectInt(int $s): void { }
<<__EntryPoint>>
function top():void {
  $x = new G(23);
  $y = new G("a");
  $z = $x->testit($y);
  expectInt($z->item);
}
