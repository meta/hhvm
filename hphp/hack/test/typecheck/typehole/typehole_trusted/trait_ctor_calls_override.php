<?hh

trait T {
  public function __construct() {
    $this->hook();
  }

  public function hook(): void {}
}

class Base {
  use T;
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
