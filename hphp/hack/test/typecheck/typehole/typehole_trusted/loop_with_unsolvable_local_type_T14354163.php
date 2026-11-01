<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class C<T> {
  public function __construct(private T $item) { }
  public function get(): T { return $this->item; }
}
class B {
  public function get(): B { return $this; }
  public function foo(): void { }
}

function expectString(string $s):void { }

function test_flow(int $n):void {
  $s = new B();
  for ($i = 0; $i < $n; $i++) {
    $s = new C($s);
  }
  // Type here is dependent on $n: it's C^$n<B>
  // But Hack infers B | C<B> | C<C<B>>
  // So if we call get() twice we end up with B
  // So it looks like we can now call foo!
  $x = $s->get()->get();
  $x->foo();
}

<<__EntryPoint>>
function main():void {
  test_flow(5);
}
