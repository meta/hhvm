<?hh

trait Slot {
  private int $x;
  public function getX(): int {
    return $this->x;
  }
}

trait Wrapper {
  use Slot;
}

class C {
  use Wrapper;
}

<<__EntryPoint>>
function main(): void {
  (new C())->getX();
}
