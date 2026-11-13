<?hh

trait T {
  private int $n;

  public function __construct() {
    $this->setUp();
    $this->n = 1;
  }

  private function setUp(): void {}
}

final class C {
  use T;

  public function setUp(): void {
    self::expectInt($this->n);
  }

  private static function expectInt(int $_): void {}
}

<<__EntryPoint>>
function main(): void {
  new C();
}
