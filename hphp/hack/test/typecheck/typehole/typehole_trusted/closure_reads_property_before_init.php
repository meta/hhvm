<?hh

final class C {
  private int $n;

  public function __construct() {
    $f = () ==> $this->n;
    self::expectInt($f());
    $this->n = 1;
  }

  private static function expectInt(int $_): void {}
}

<<__EntryPoint>>
function main(): void {
  new C();
}
