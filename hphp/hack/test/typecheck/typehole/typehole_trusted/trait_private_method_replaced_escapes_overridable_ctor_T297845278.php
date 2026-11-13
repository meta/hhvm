<?hh

trait T {
  public function __construct() {
    $this->setUp();
  }

  private function setUp(): void {}
}

class Base {
  use T;

  public function setUp(): void {
    $this->hook();
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
