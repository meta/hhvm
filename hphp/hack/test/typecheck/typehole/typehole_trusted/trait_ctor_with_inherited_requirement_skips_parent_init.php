<?hh

<<__ConsistentConstruct>>
class P {
  public function __construct(public int $x = 0) {}
}

trait Requirement {
  require extends P;
}

trait T {
  use Requirement;
  public function __construct(int $y = 0): void {}
}

class C extends P {
  use T;
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new C())->x);
}
