<?hh

class P {
  public function __construct(public int $x): void {}
}

trait T {
  public function __construct(): void {}
}

abstract class A extends P {
  use T;
}

class B extends A {}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new B())->x);
}
