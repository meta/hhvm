<?hh

trait Slot {
  public int $x;
}

abstract class A {
  use Slot;
}

class B extends A {}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new B())->x);
}
