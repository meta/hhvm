<?hh

// The parent constructor calls an overridden hook before the child constructor
// sets $n; init checks don't follow virtual calls made from a parent ctor.
class Break8 {
  public static function run(): int {
    new Break8Child();
    return 0;
  }
}

class Break8Base {
  public function __construct() {
    $this->hook();
  }
  public function hook(): void {}
}

final class Break8Child extends Break8Base {
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
  Break8::run();
}
