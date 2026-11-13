<?hh

class Base {
  public function __construct() {
    $this->visit(null);
    $this->visit($this);
  }

  private function visit(?Base $b): void {
    $b?->hook();
  }

  public function hook(): void {}
}

final class Child extends Base {
  private int $n;

  public function __construct() {
    parent::__construct();
    $this->n = 1;
  }

  <<__Override>>
  public function hook(): void {
    self::expectInt($this->n);
  }

  private static function expectInt(int $_): void {}
}

<<__EntryPoint>>
function main(): void {
  new Child();
}
