<?hh

trait PrivateSlot {
  private int $value;
  public function __construct(): void {
    $this->value = 42;
  }
}

class P {
  use PrivateSlot;
  public function parentValue(): int {
    return $this->value;
  }
}

class C extends P {
  use PrivateSlot;
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new C())->parentValue());
}
