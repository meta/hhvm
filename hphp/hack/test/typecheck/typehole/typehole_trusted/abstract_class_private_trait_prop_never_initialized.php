<?hh

trait Slot {
  private int $x;
  public function getX(): int {
    return $this->x;
  }
}

abstract class A {
  use Slot;
}

final class B extends A {
  public function __construct() {}
}

<<__EntryPoint>>
function main(): void {
  (new B())->getX();
}
