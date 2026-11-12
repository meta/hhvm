<?hh

class P {
  public function __construct(public int $x): void {}
}

trait T {
  public function __construct(): void {}
}

trait Wrapper {
  use T;
}

class C extends P {
  use Wrapper;
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new C())->x);
}
