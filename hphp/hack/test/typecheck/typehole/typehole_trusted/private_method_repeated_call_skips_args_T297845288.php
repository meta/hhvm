<?hh

final class C {
  private int $n;

  public function __construct() {
    $this->visit(null);
    $this->visit($this);
    $this->n = 1;
  }

  private function visit(?C $c): void {
    $c?->read();
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
