<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__UNSAFE_AllowMultipleInstantiations>>
interface A<T> {}

interface B<T> extends A<T> {
  public function f(): T;
}

function expect_int(int $_): void {}

function test(A<int> $a): void {
  if ($a is B<_>) {
    expect_int($a->f());
  }
}

class C implements A<int>, B<string> {
  public function f(): string {
    return 'foo';
  }
}

<<__EntryPoint>>
function main(): void {
  test(new C());
}
