<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

final class ReifyTest<<<__Enforceable>> reify T> {
  public function __construct(private T $val) {}
  public function foo(mixed $other): T {
    $other as this;
    return $other->val;
  }
}

<<__EntryPoint>>
function breakit(): void {
  $x = new ReifyTest<int>(42);
  $x->foo(new ReifyTest<string>('bar'));
}
