<?hh

final class C {
  private int $n;

  public function __construct() {
    $this->init();
    $this->read();
  }

  private function init(): Generator<int, int, void> {
    $this->n = 1;
    yield 1;
  }

  public function read(): void {
    self::expectInt($this->n);
  }

  private static function expectInt(int $_): void {}
}

<<__EntryPoint>>
function main(): void {
  new C();
}
